/* =====================================================================
   Silviozas Server (Remake) - standalone backend for the Silviozas
   Premium Proxy control channel ("Proxy Server" the client connects to).

   Transport: Growtopia-style ENet
       checksum = enet_crc32, compress = range coder,
       OLD packet header (usingNewPacket = 0), port 33131.

   Application: type-4 "SilviozasPacket", 4-byte XOR encrypted.
       [0..3]   type = 04 00 00 00        (clear)
       [4..7]   key  = 4 random bytes      (clear)
       [8..27]  header fields  XOR key[i%4]   (field_8 = 1, rest 0)
       [28..31] m_data_size               (clear)
       [32..]   payload  XOR key[i%4]
       payload plaintext = [01][uint16 len][text]

   Handled commands:
       [OnNewLogin]/[USER]/...     -> "Ping Success."
       [GetServerData]            -> "[ServerData]\nmaint|..\nserver|..\nport|.."
       [GetMeta]/[GetMetaSign]    -> "[Meta]/[MetaSign]<value>" (from file, else ack)
       [CheckItemVendPrices]/...  -> "[ItemPricesList]\n<data>" (from file, else ack)
       [OnModDetect]              -> aggregate hidden mods
       [OnGiveMods]               -> "[OnDialogRequest]" hidden-moderator dialog

   Server -> client extras:
       [PROXYCONSOLE]\n<text>  -> line in the proxy's exe console
       [OVERLAY]\n<text>       -> big text on the game screen

   Discord: .selahattingt
   ===================================================================== */

#include <iostream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <conio.h>
#include <enet/enet.h>
#include "utils.h"

// ------------------------- configuration ------------------------------
static const char*    BIND_HOST      = "0.0.0.0";
static const uint16_t BIND_PORT      = 33131;
static const bool     USE_NEW_PACKET = false;   // matches the client's enet header

// [ServerData] payload - the Growtopia server the proxy routes through.
// Editable at runtime via serverdata.txt:  "maint|server_ip|port".
static std::string    g_maint    = "0";
static std::string    g_gtserver = "213.179.209.175";
static std::string    g_gtport   = "17044";

// exe-console line (editable live via send.txt), broadcast every 60s.
static const char* DEFAULT_CONSOLE_MSG = "`2Silviozas By `4.selahattingt `9Dont Sell Another Players!!";
// in-game overlay line (editable live via overlay.txt), broadcast every 10 min.
static const char* DEFAULT_OVERLAY_MSG = "`4Proxy Owner Discord: `2.selahattingt``";

ENetHost* g_host    = nullptr;
int       g_clients = 0;

static void update_title() {
	char title[128];
	snprintf(title, sizeof(title), "Silviozas Server  |  Clients: %d  |  Port %u", g_clients, BIND_PORT);
	SetConsoleTitleA(title);
}

// --------------------- type-4 XOR crypto ------------------------------
static void silv_xor(uint8_t* d, size_t len) {
	if (len < 32) return;
	uint8_t key[4] = { d[4], d[5], d[6], d[7] };
	int32_t size = *(int32_t*)(d + 28);
	for (size_t i = 8; i < 28 && i < len; i++) d[i] ^= key[i % 4];
	size_t end = 32 + (size_t)size; if (end > len) end = len;
	for (size_t i = 32; i < end; i++) d[i] ^= key[i % 4];
}

static std::string silv_decode_text(ENetPacket* p) {
	if (!p || p->dataLength < 36 || p->data[0] != 4) return std::string();
	std::vector<uint8_t> buf(p->data, p->data + p->dataLength);
	silv_xor(buf.data(), buf.size());
	int32_t size = *(int32_t*)(buf.data() + 28);
	if (size < 3 || 32 + size > (int)buf.size()) return std::string();
	uint16_t tlen = buf[33] | (buf[34] << 8);
	size_t off = 35;
	if (off + tlen > buf.size()) tlen = (uint16_t)(buf.size() - off);
	return std::string((char*)buf.data() + off, tlen);
}

static ENetPacket* silv_build(const std::string& text) {
	uint16_t tlen = (uint16_t)text.size();
	int32_t size = 3 + tlen;
	size_t total = 33 + size;
	ENetPacket* p = enet_packet_create(0, total, ENET_PACKET_FLAG_RELIABLE);
	uint8_t* d = p->data;
	memset(d, 0, total);
	d[0] = 4;
	uint8_t key[4] = { (uint8_t)utils::random(1,255), (uint8_t)utils::random(1,255),
	                   (uint8_t)utils::random(1,255), (uint8_t)utils::random(1,255) };
	d[4]=key[0]; d[5]=key[1]; d[6]=key[2]; d[7]=key[3];
	*(int32_t*)(d + 8)  = 1;
	*(int32_t*)(d + 28) = size;
	d[32] = 1;
	d[33] = tlen & 0xFF; d[34] = (tlen >> 8) & 0xFF;
	memcpy(d + 35, text.data(), tlen);
	silv_xor(d, total);
	return p;
}

static void send_text(ENetPeer* peer, const std::string& text) {
	enet_peer_send(peer, 0, silv_build(text));
	enet_host_flush(g_host);
	PRINTS("S->C: %s\n", text.c_str());
}
static void send_console(ENetPeer* peer, const std::string& text) { send_text(peer, "[PROXYCONSOLE]\n" + text); }
static void send_overlay(ENetPeer* peer, const std::string& text) { send_text(peer, "[OVERLAY]\n" + text); }

static uint32_t peer_ipv4(ENetPeer* pr) {
	return (uint32_t)pr->address.host.v4[0]        | ((uint32_t)pr->address.host.v4[1] << 8)
	     | ((uint32_t)pr->address.host.v4[2] << 16) | ((uint32_t)pr->address.host.v4[3] << 24);
}

// Send one payload to every connected client, once per unique IP.
static int broadcast(const std::string& payload) {
	uint32_t seen[512]; int ns = 0;
	for (size_t i = 0; i < g_host->peerCount; i++) {
		ENetPeer* pr = &g_host->peers[i];
		if (pr->state != ENET_PEER_STATE_CONNECTED) continue;
		uint32_t ip = peer_ipv4(pr);
		bool dup = false;
		for (int k = 0; k < ns; k++) if (seen[k] == ip) { dup = true; break; }
		if (dup) continue;
		if (ns < 512) seen[ns++] = ip;
		enet_peer_send(pr, 0, silv_build(payload));
	}
	enet_host_flush(g_host);
	return ns;
}

// --------------------- runtime config files ---------------------------
static std::string read_file_trim(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return std::string();
	std::string s; char c;
	while (fread(&c, 1, 1, f) == 1) s += c;
	fclose(f);
	while (!s.empty() && (s.back()=='\n' || s.back()=='\r' || s.back()==' ')) s.pop_back();
	return s;
}
static void load_serverdata() {
	std::string s = read_file_trim("serverdata.txt");   // maint|server_ip|port
	if (s.empty()) return;
	size_t a = s.find('|'), b = (a==std::string::npos)?a:s.find('|', a+1);
	if (a!=std::string::npos && b!=std::string::npos) {
		g_maint = s.substr(0,a); g_gtserver = s.substr(a+1,b-a-1); g_gtport = s.substr(b+1);
	}
}
static std::string server_data_reply() { return "[ServerData]\nmaint|"+g_maint+"\nserver|"+g_gtserver+"\nport|"+g_gtport; }
static std::string console_message() { std::string s=read_file_trim("send.txt");    return s.empty()?std::string(DEFAULT_CONSOLE_MSG):s; }
static std::string overlay_message() { std::string s=read_file_trim("overlay.txt"); return s.empty()?std::string(DEFAULT_OVERLAY_MSG):s; }

// --------------------- hidden moderator checker -----------------------
struct ModHit { std::string mod, real, world; time_t t; };
std::vector<ModHit> g_mods;

static std::string field_value(const std::string& body, const char* key) {
	std::string needle = std::string(key) + "|";
	size_t pos = body.find(needle);
	if (pos == std::string::npos) return std::string();
	pos += needle.size();
	size_t end = body.find('\n', pos);
	return body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}
static std::string time_ago(time_t t) {
	long s = (long)(time(nullptr) - t); if (s < 0) s = 0;
	long d = s/86400; s%=86400; long h = s/3600; s%=3600; long m = s/60;
	char buf[64];
	if (d>0)      snprintf(buf,sizeof(buf),"%ld day%s, %ld hour%s", d,d==1?"":"s", h,h==1?"":"s");
	else if (h>0) snprintf(buf,sizeof(buf),"%ld hour%s, %ld min", h,h==1?"":"s", m);
	else          snprintf(buf,sizeof(buf),"%ld min", m);
	return buf;
}
static void mods_save() {
	FILE* f = fopen("hdnmods.txt","w"); if (!f) return;
	for (auto& e : g_mods) fprintf(f,"%s|%s|%s|%lld\n", e.mod.c_str(), e.real.c_str(), e.world.c_str(), (long long)e.t);
	fclose(f);
}
static void mods_load() {
	g_mods.clear();
	FILE* f = fopen("hdnmods.txt","r"); if (!f) return;
	char line[512];
	while (fgets(line,sizeof(line),f)) {
		std::string s(line);
		while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
		if (s.empty()) continue;
		ModHit e; e.t = time(nullptr);
		size_t a=s.find('|'), b=a==std::string::npos?a:s.find('|',a+1), c=b==std::string::npos?b:s.find('|',b+1);
		if (a==std::string::npos) e.mod=s;
		else {
			e.mod=s.substr(0,a);
			e.real=s.substr(a+1,(b==std::string::npos?s.size():b)-a-1);
			if (b!=std::string::npos) e.world=s.substr(b+1,(c==std::string::npos?s.size():c)-b-1);
			if (c!=std::string::npos) e.t=(time_t)atoll(s.c_str()+c+1);
		}
		g_mods.push_back(e);
	}
	fclose(f);
}
static void mods_add(const std::string& mod, const std::string& real, const std::string& world) {
	if (mod.empty() && world.empty()) return;
	for (auto& e : g_mods)
		if (e.mod==mod && e.world==world) { e.t=time(nullptr); if(!real.empty()) e.real=real; mods_save(); return; }
	g_mods.push_back({mod,real,world,time(nullptr)});
	mods_save();
}
static std::string hidden_mod_dialog() {
	std::string d = "[OnDialogRequest]\n";
	d += "add_label_with_icon|big|`2Hidden Moderator Checker|left|732|\n";
	d += "add_spacer|small|\nadd_quick_exit|\n";
	d += "add_textbox|`9Hidden `#@Moderators`` Detected With Proxy:|\nadd_spacer|small|\n";
	for (auto& e : g_mods) {
		std::string real = e.real.empty() ? e.mod : e.real;
		d += "add_label_with_icon_button||`#@" + e.mod + "`` `9[```w" + real + "```9] Detected In `w" + e.world
		   + "`` " + time_ago(e.t) + " ago!|left|546|" + e.world + "|\n";
	}
	d += "add_spacer|small|\nend_dialog|hdnmod|Cancel||";
	return d;
}

// ------------------------- request dispatch ---------------------------
static void handle_request(ENetPeer* peer, ENetPacket* p) {
	std::string t = silv_decode_text(p);
	if (t.empty()) return;
	PRINTS("C->S: %s\n", t.substr(0, t.find('\n')).c_str());

	if (t.rfind("[GetServerData]", 0) == 0) { send_text(peer, server_data_reply()); return; }

	if (t.rfind("[OnModDetect]", 0) == 0) {
		std::string mod = field_value(t,"Name"), world = field_value(t,"WorldName"), real = field_value(t,"RealName");
		if (mod.empty()) { size_t nl=t.find('\n'); if (nl!=std::string::npos) mod=t.substr(nl+1, t.find('\n',nl+1)-nl-1); }
		mods_add(mod, real, world);
		PRINTS("mod detected: %s @ %s (total %zu)\n", mod.c_str(), world.c_str(), g_mods.size());
		send_text(peer, "Ping Success."); return;
	}
	if (t.rfind("[OnGiveMods]", 0) == 0) { send_text(peer, hidden_mod_dialog()); return; }

	if (t.rfind("[GetMeta]", 0) == 0)     { std::string m=read_file_trim("meta.txt");     send_text(peer, m.empty()?std::string("Ping Success."):"[Meta]"+m); return; }
	if (t.rfind("[GetMetaSign]", 0) == 0) { std::string m=read_file_trim("metasign.txt"); send_text(peer, m.empty()?std::string("Ping Success."):"[MetaSign]"+m); return; }

	if (t.rfind("[CheckItemVendPrices]", 0) == 0 || t.rfind("[CheckAverageItemPrices]", 0) == 0) {
		std::string pr = read_file_trim("itemprices.txt");
		send_text(peer, pr.empty()?std::string("Ping Success."):"[ItemPricesList]\n"+pr); return;
	}
	if (t.rfind("[OnSetClothing]", 0) == 0 || t.rfind("[OnStopVisualClothes]", 0) == 0) { send_text(peer, "Ping Success."); return; }

	// [OnNewLogin], [USER], [OnVendingInfo], captcha, token hashes, etc.
	send_text(peer, "Ping Success.");
}

int main() {
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (enet_initialize() != 0) { std::cerr << "Could not initialize ENet\n"; return EXIT_FAILURE; }
	atexit(enet_deinitialize);

	load_serverdata();
	mods_load();

	ENetAddress address;
	address.type = ENET_ADDRESS_TYPE_IPV4;
	enet_address_set_host_ip(&address, BIND_HOST);
	address.port = BIND_PORT;
	g_host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, &address, 128, 2, 0, 0);
	if (!g_host) { printf("Failed to bind %s:%d\n", BIND_HOST, BIND_PORT); Sleep(-1); return 0; }

	g_host->checksum = enet_crc32;
	g_host->usingNewPacket = USE_NEW_PACKET;
	g_host->usingNewPacketForServer = USE_NEW_PACKET;
	if (enet_host_compress_with_range_coder(g_host) != 0) PRINTS("range coder setup failed\n");

	printf("Silviozas Server listening on %s:%u\n", BIND_HOST, BIND_PORT);
	printf("  ServerData -> maint|%s server|%s port|%s\n", g_maint.c_str(), g_gtserver.c_str(), g_gtport.c_str());
	update_title();

	std::string input_buf;
	while (true) {
		Sleep(1);
		ENetEvent ev;
		ULONGLONG now = GetTickCount64();

		// every 60s: exe-console lines to all clients
		static ULONGLONG last_console = 0;
		if (g_clients > 0 && now - last_console >= 60000) {
			last_console = now;
			broadcast("[PROXYCONSOLE]\n" + console_message());
			broadcast("[PROXYCONSOLE]\n`9Owner Discord: `2.selahattingt");
		}
		// every 10 min: in-game overlay to all clients
		static ULONGLONG last_overlay = 0;
		if (g_clients > 0 && now - last_overlay >= 600000) {
			last_overlay = now;
			int n = broadcast("[OVERLAY]\n" + overlay_message());
			PRINTS("overlay broadcast to %d client(s)\n", n);
		}
		// type a line in THIS console + Enter -> broadcast it as an overlay now
		while (_kbhit()) {
			int ch = _getch();
			if (ch == '\r' || ch == '\n') {
				putchar('\n');
				if (!input_buf.empty()) {
					int n = broadcast("[OVERLAY]\n" + input_buf);
					PRINTS("manual overlay -> %d client(s): %s\n", n, input_buf.c_str());
					input_buf.clear();
				}
			} else if (ch == 8) { if (!input_buf.empty()) { input_buf.pop_back(); printf("\b \b"); } }
			else if (ch >= 32 && ch < 127) { input_buf += (char)ch; putchar(ch); }
		}

		while (enet_host_service(g_host, &ev, 0) > 0) {
			switch (ev.type) {
			case ENET_EVENT_TYPE_CONNECT: {
				char ip[64] = {0};
				enet_address_get_host_ip(&ev.peer->address, ip, sizeof(ip));
				g_clients++;
				PRINTS("client connected from %s:%u  ->  Clients: %d\n", ip, ev.peer->address.port, g_clients);
				update_title();
			} break;
			case ENET_EVENT_TYPE_RECEIVE:
				if (ev.packet->dataLength && ev.packet->data[0] == 4)
					handle_request(ev.peer, ev.packet);
				enet_packet_destroy(ev.packet);
				break;
			case ENET_EVENT_TYPE_DISCONNECT:
				if (g_clients > 0) g_clients--;
				PRINTS("client disconnected  ->  Clients: %d\n", g_clients);
				update_title();
				break;
			default: break;
			}
		}
	}
}
