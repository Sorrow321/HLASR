#include "voice_capture.h"
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <sstream>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <ogg/ogg.h>
#include <speex/speex.h>
#include <speex/speex_header.h>
#endif
#include "enginecallbacks.h"

static std::unordered_map<int, std::vector<unsigned char>> g_playerVoiceBuffers;

extern "C" {
	extern cvar_t vx_debug;
	extern cvar_t vx_spx;
	extern cvar_t vx_rate;
	extern cvar_t vx_codec;
	extern cvar_t vx_opus_rate;
}

// Hex helper for debug
static void debug_hex_dump(const unsigned char* buf, unsigned int len, unsigned int maxBytes)
{
	if (!buf || len == 0) return;
	char line[512];
	unsigned int n = (len < maxBytes) ? len : maxBytes;
	unsigned int pos = 0;
	for (unsigned int i = 0; i < n; ++i) {
		int wrote = std::snprintf(line + pos, sizeof(line) - pos, "%02X ", buf[i]);
		if (wrote <= 0) break;
		pos += (unsigned)wrote;
		if (pos > sizeof(line) - 8) break;
	}
	line[pos] = '\n';
	line[pos + 1] = '\0';
	SERVER_PRINT(line);
}

struct PlayerVoiceState
{
	bool isRecording = false;
	double lastVoiceTime = 0.0;
	double segmentStartTime = 0.0;
	std::vector<std::vector<unsigned char>> packets;
};

static std::unordered_map<int, PlayerVoiceState> g_playerVoiceState;
static const double kSilenceCloseSeconds = 0.6;

static void ensure_directory_chain(const std::string &path)
{
	if (path.empty())
		return;

	size_t pos = 0;
	do {
		pos = path.find('/', pos + 1);
		std::string sub = path.substr(0, pos);
		if (sub.empty())
			continue;
#ifdef _WIN32
		_mkdir(sub.c_str());
#else
		mkdir(sub.c_str(), 0755);
#endif
	} while (pos != std::string::npos);
}

static std::string sanitize_filename(const std::string &s)
{
	std::string out = s;
	for (char &ch : out) {
		if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
			ch = '_';
	}
	return out;
}

static std::string build_output_path(IGameClient *client, const char *ext)
{
	char gameDir[512] = {0};
	g_engfuncs.pfnGetGameDir(gameDir);

	edict_t *pEdict = client ? client->GetEdict() : nullptr;
	const char *auth = pEdict ? GETPLAYERAUTHID(pEdict) : nullptr;
	if (!auth) auth = "UNKNOWN";
	std::string steamid = sanitize_filename(auth);

	std::time_t t = std::time(nullptr);
	char tsbuf[32];
	std::strftime(tsbuf, sizeof(tsbuf), "%Y%m%d-%H%M%S", std::localtime(&t));

	std::ostringstream oss;
	oss << gameDir << "/data/voice_logs/" << steamid << "/" << tsbuf << ext;
	return oss.str();
}

static std::string netadr_to_string(const netadr_t *adr)
{
	if (!adr) return "";
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", adr->ip[0], adr->ip[1], adr->ip[2], adr->ip[3], (unsigned)adr->port);
	return std::string(buf);
}

#ifndef _WIN32
static bool write_ogg_speex(const std::vector<std::vector<unsigned char>>& packets, const std::string& outPath, int sampleRate)
{
	ogg_stream_state os;
	ogg_page og;
	ogg_packet op = {};
	int strip = 0;
	if (CVAR_GET_FLOAT) {
		int s = (int)CVAR_GET_FLOAT("vx_strip");
		if (s > 0 && s < 1024) strip = s;
	}
	int serial = (int)((uintptr_t)&os ^ (uintptr_t)outPath.c_str());
	if (ogg_stream_init(&os, serial) != 0)
		return false;

	// Speex header
	SpeexHeader header;
	speex_init_header(&header, sampleRate, 1, &speex_nb_mode);
	header.frames_per_packet = 1;
	header.vbr = 0;
	header.nb_channels = 1;
	header.bitrate = -1;
	header.frame_size = header.frame_size > 0 ? header.frame_size : 160;

	int header_size = 0;
	char* header_data = speex_header_to_packet(&header, &header_size);
	if (!header_data || header_size <= 0) {
		ogg_stream_clear(&os);
		return false;
	}

	FILE *f = fopen(outPath.c_str(), "wb");
	if (!f) {
		ogg_stream_clear(&os);
		if (header_data) speex_header_free(header_data);
		return false;
	}

	// Write header packet
	memset(&op, 0, sizeof(op));
	op.packet = (unsigned char*)header_data;
	op.bytes = (long)header_size;
	op.b_o_s = 1;
	op.e_o_s = 0;
	op.granulepos = 0;
	ogg_stream_packetin(&os, &op);
	while (ogg_stream_flush(&os, &og)) {
		fwrite(og.header, 1, og.header_len, f);
		fwrite(og.body, 1, og.body_len, f);
	}

	// free header_data after it's flushed
	if (header_data) speex_header_free(header_data);

	// Write minimal comment packet
	const char* vendor = "voice_export";
	unsigned char cmdbuf[64];
	memset(&op, 0, sizeof(op));
	uint32_t vlen = (uint32_t)strlen(vendor);
	if (vlen > sizeof(cmdbuf) - 8) vlen = sizeof(cmdbuf) - 8;
	unsigned char *p = cmdbuf;
	memcpy(p, &vlen, 4); p += 4;
	memcpy(p, vendor, vlen); p += vlen;
	uint32_t ncomments = 0;
	memcpy(p, &ncomments, 4); p += 4;
	op.packet = cmdbuf;
	op.bytes = (long)(p - cmdbuf);
	op.b_o_s = 0;
	op.e_o_s = 0;
	op.granulepos = 0;
	ogg_stream_packetin(&os, &op);
	while (ogg_stream_flush(&os, &og)) {
		fwrite(og.header, 1, og.header_len, f);
		fwrite(og.body, 1, og.body_len, f);
	}

	// Data packets
	long granule = 0;
	for (const auto& pkt : packets) {
		if (pkt.size() <= (size_t)strip)
			continue;
		memset(&op, 0, sizeof(op));
		op.packet = (unsigned char*)(pkt.data() + strip);
		op.bytes = (long)(pkt.size() - strip);
		op.b_o_s = 0;
		op.e_o_s = 0;
		granule += header.frame_size;
		op.granulepos = granule;
		ogg_stream_packetin(&os, &op);
		while (ogg_stream_pageout(&os, &og)) {
			fwrite(og.header, 1, og.header_len, f);
			fwrite(og.body, 1, og.body_len, f);
		}
	}

	// End of stream
	memset(&op, 0, sizeof(op));
	op.e_o_s = 1;
	ogg_stream_packetin(&os, &op);
	while (ogg_stream_flush(&os, &og)) {
		fwrite(og.header, 1, og.header_len, f);
		fwrite(og.body, 1, og.body_len, f);
	}

	fclose(f);
	ogg_stream_clear(&os);
	return true;
}

static void le16(uint16_t v, unsigned char *p) { p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF); }
static void le32(uint32_t v, unsigned char *p) { p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF); p[2] = (unsigned char)((v >> 16) & 0xFF); p[3] = (unsigned char)((v >> 24) & 0xFF); }

static bool write_ogg_opus(const std::vector<std::vector<unsigned char>>& packets, const std::string& outPath, int inputRate)
{
	ogg_stream_state os;
	ogg_page og;
	ogg_packet op = {};
	int serial = (int)((uintptr_t)&os ^ (uintptr_t)outPath.c_str());
	if (ogg_stream_init(&os, serial) != 0)
		return false;

	FILE *f = fopen(outPath.c_str(), "wb");
	if (!f) {
		ogg_stream_clear(&os);
		return false;
	}

	// OpusHead
	unsigned char head[19] = {0};
	memcpy(head, "OpusHead", 8);
	head[8] = 1;           // version
	head[9] = 1;           // channels
	le16(0, head + 10);    // pre-skip
	le32((uint32_t)inputRate, head + 12); // input rate hint
	le16(0, head + 16);    // output gain
	head[18] = 0;          // channel mapping

	memset(&op, 0, sizeof(op));
	op.packet = head;
	op.bytes = sizeof(head);
	op.b_o_s = 1;
	op.e_o_s = 0;
	op.granulepos = 0;
	ogg_stream_packetin(&os, &op);
	while (ogg_stream_flush(&os, &og)) {
		fwrite(og.header, 1, og.header_len, f);
		fwrite(og.body, 1, og.body_len, f);
	}

	// OpusTags
	const char *vendor = "voice_export";
	unsigned char tags[64] = {0};
	memcpy(tags, "OpusTags", 8);
	uint32_t vlen = (uint32_t)strlen(vendor);
	unsigned char *tp = tags + 8;
	le32(vlen, tp); tp += 4;
	memcpy(tp, vendor, vlen); tp += vlen;
	le32(0, tp); tp += 4; // user comments = 0

	memset(&op, 0, sizeof(op));
	op.packet = tags;
	op.bytes = (long)(tp - tags);
	op.b_o_s = 0;
	op.e_o_s = 0;
	op.granulepos = 0;
	ogg_stream_packetin(&os, &op);
	while (ogg_stream_flush(&os, &og)) {
		fwrite(og.header, 1, og.header_len, f);
		fwrite(og.body, 1, og.body_len, f);
	}

	// Data packets (assume 20ms frames @ 48k => 960 samples per packet)
	long granule = 0;
	const int opus_samples_per_packet = 960;
	for (const auto& pkt : packets) {
		if (pkt.empty())
			continue;
		memset(&op, 0, sizeof(op));
		op.packet = (unsigned char*)pkt.data();
		op.bytes = (long)pkt.size();
		op.b_o_s = 0;
		op.e_o_s = 0;
		granule += opus_samples_per_packet;
		op.granulepos = granule;
		ogg_stream_packetin(&os, &op);
		while (ogg_stream_pageout(&os, &og)) {
			fwrite(og.header, 1, og.header_len, f);
			fwrite(og.body, 1, og.body_len, f);
		}
	}

	memset(&op, 0, sizeof(op));
	op.e_o_s = 1;
	ogg_stream_packetin(&os, &op);
	while (ogg_stream_flush(&os, &og)) {
		fwrite(og.header, 1, og.header_len, f);
		fwrite(og.body, 1, og.body_len, f);
	}

	fclose(f);
	ogg_stream_clear(&os);
	return true;
}
#endif

// Hook: HandleNetCommand
static void OnHandleNetCommand(IVoidHookChain<IGameClient*, int8>* chain, IGameClient* client, int8 cmd)
{
	if (!g_RehldsFuncs)
		return chain->callNext(client, cmd);

	sizebuf_t *msg = g_RehldsFuncs->GetNetMessage();
	int *pReadCount = g_RehldsFuncs->GetMsgReadCount();
	int beforeRead = pReadCount ? *pReadCount : 0;
	double beforeVoice = client->GetLastVoiceTime();

	// Snapshot per-client datagram sizes before other plugins run
	std::vector<sizebuf_t*> dgList;
	std::vector<int>        dgPrev;
	if (g_RehldsSvs) {
		int maxc = g_RehldsSvs->GetMaxClients();
		dgList.resize(maxc, nullptr);
		dgPrev.resize(maxc, 0);
		for (int i = 0; i < maxc; ++i) {
			IGameClient *dst = g_RehldsSvs->GetClient(i);
			if (!dst) continue;
			sizebuf_t *dg = dst->GetDatagram();
			dgList[i] = dg;
			dgPrev[i] = dg ? dg->cursize : 0;
		}
	}

	// Capture upstream payload when opcode matches clc_voicedata (8)
	if (cmd == 8 && msg && msg->data && beforeRead + 2 <= msg->cursize) {
		int payloadLen = (int)msg->data[beforeRead] | ((int)msg->data[beforeRead + 1] << 8);
		if (payloadLen > 0 && beforeRead + 2 + payloadLen <= msg->cursize) {
			int id = client->GetId();
			// append to raw buffer
			auto &buf = g_playerVoiceBuffers[id];
			buf.insert(buf.end(), msg->data + beforeRead + 2, msg->data + beforeRead + 2 + payloadLen);

			// packet list for container mux
			auto &st = g_playerVoiceState[id];
			if (!st.isRecording) {
				st.isRecording = true;
				st.segmentStartTime = beforeVoice;
				const char *nm = client->GetName();
				char info[256];
				std::snprintf(info, sizeof(info), "[voice_export] REC START: id=%d name=\"%s\"\n", id, nm ? nm : "");
				SERVER_PRINT(info);
			}
		// Use current server time for silence detection
		double nowTs = 0.0;
		if (g_RehldsApi) {
			if (auto svd = g_RehldsApi->GetServerData()) {
				nowTs = svd->GetTime();
			}
		}
		st.lastVoiceTime = nowTs;
			st.packets.emplace_back();
			auto &pkt = st.packets.back();
			pkt.insert(pkt.end(), msg->data + beforeRead + 2, msg->data + beforeRead + 2 + payloadLen);
		}
	}

	chain->callNext(client, cmd);

	double afterVoice = client->GetLastVoiceTime();
	int afterRead = pReadCount ? *pReadCount : beforeRead;

	// Capture downstream Speex (server -> clients) appended into per-client datagrams during this call
	bool capturedDownstream = false;
	if (!dgList.empty()) {
		for (size_t i = 0; i < dgList.size(); ++i) {
			sizebuf_t *dg = dgList[i];
			if (!dg) continue;
			int from = dgPrev[i];
			int to   = dg->cursize;
			if (!dg->data || to <= from) continue;
			unsigned char *base = dg->data;
			int j = from;
			while (j + 4 <= to) {
				// svc_voicedata = 53
				if (base[j] == 53) {
					if (j + 4 > to) break;
					int vid = base[j + 1];
					int vlen = (int)base[j + 2] | ((int)base[j + 3] << 8);
					if (vlen <= 0 || j + 4 + vlen > to) break;
					// Append to that player's buffer and packet list
					auto &buf = g_playerVoiceBuffers[vid];
					buf.insert(buf.end(), base + j + 4, base + j + 4 + vlen);
					auto &st = g_playerVoiceState[vid];
					if (!st.isRecording) {
						st.isRecording = true;
						st.segmentStartTime = beforeVoice;
						const char *nm = nullptr;
						if (g_RehldsSvs) {
							IGameClient *c = g_RehldsSvs->GetClient(vid);
							nm = c ? c->GetName() : nullptr;
						}
						char info[256];
						std::snprintf(info, sizeof(info), "[voice_export] REC START (down): id=%d name=\"%s\"\n", (int)vid, nm ? nm : "");
						SERVER_PRINT(info);
					}
					// time now for silence detection
					double nowTs = 0.0;
					if (g_RehldsApi) {
						if (auto svd = g_RehldsApi->GetServerData()) nowTs = svd->GetTime();
					}
					st.lastVoiceTime = nowTs;
					st.packets.emplace_back();
					auto &pkt = st.packets.back();
					pkt.insert(pkt.end(), base + j + 4, base + j + 4 + vlen);
					j += 4 + vlen;
					capturedDownstream = true;
					continue;
				}
				++j;
			}
		}
	}

	if (!capturedDownstream && afterVoice > beforeVoice && msg && afterRead > beforeRead && msg->data && afterRead <= msg->cursize) {
		int consumed = afterRead - beforeRead;
		int id = client->GetId();
		auto &buf = g_playerVoiceBuffers[id];
		buf.insert(buf.end(), msg->data + beforeRead, msg->data + beforeRead + consumed);

		// Mark recording started/continued
		auto &st = g_playerVoiceState[id];
		if (!st.isRecording)
		{
			st.isRecording = true;
			st.segmentStartTime = afterVoice;

			const char *nm = client->GetName();
			char info[256];
			std::snprintf(info, sizeof(info), "[voice_export] REC START: id=%d name=\"%s\"\n", id, nm ? nm : "");
			SERVER_PRINT(info);
		}
		st.lastVoiceTime = afterVoice;
		// keep packet boundary for container muxing
		st.packets.emplace_back();
		auto &pkt = st.packets.back();
		pkt.insert(pkt.end(), msg->data + beforeRead, msg->data + beforeRead + consumed);
	} else {
		// Optional debug for troubleshooting voice detection
		if (CVAR_GET_FLOAT && CVAR_GET_FLOAT("vx_debug") >= 1.0f) {
			char dbg[256];
			std::snprintf(dbg, sizeof(dbg),
				"[voice_export] dbg HandleNetCommand: cmd=%d consumed=%d voice %.2f->%.2f\n",
				(int)cmd, (pReadCount ? (*pReadCount - beforeRead) : -1), beforeVoice, afterVoice);
			SERVER_PRINT(dbg);
		}
	}
}

void VoiceCapture_RegisterHooks()
{
	if (g_RehldsHookchains) {
		// High priority so we see packets before other plugins short-circuit
		g_RehldsHookchains->HandleNetCommand()->registerHook(OnHandleNetCommand, HC_PRIORITY_HIGH);
		// Also observe raw client packets for debugging/voicedata signature
		g_RehldsHookchains->PreprocessPacket()->registerHook([](IHookChain<bool, uint8*, unsigned int, const netadr_t&>* chain, uint8* data, unsigned int len, const netadr_t& from) -> bool {
			// Only print when explicitly requested
			if (CVAR_GET_FLOAT && CVAR_GET_FLOAT("vx_debug") >= 2.0f) {
				char hdr[128];
				std::snprintf(hdr, sizeof(hdr), "[voice_export] dbg PreprocessPacket: len=%u from %u.%u.%u.%u:%u\n",
					len, from.ip[0], from.ip[1], from.ip[2], from.ip[3], (unsigned)from.port);
				SERVER_PRINT(hdr);
				debug_hex_dump((const unsigned char*)data, len, 64);
			}
			return chain->callNext(data, len, from);
		});
	}
}

void VoiceCapture_UnregisterHooks()
{
	if (g_RehldsHookchains) {
		g_RehldsHookchains->HandleNetCommand()->unregisterHook(OnHandleNetCommand);
		// Note: lambda unregister not supported; safe since plugin unload resets hookchains
	}
}

// Server commands
static void Cmd_VoiceSegmentStart(void)
{
	if (!g_RehldsSvs) {
		SERVER_PRINT("[voice_export] ReHLDS not available\n");
		return;
	}
	if (CMD_ARGC() < 2) {
		SERVER_PRINT("Usage: voice_segment_start <player_index>\n");
		return;
	}
	int userIdx = atoi(CMD_ARGV(1));
	int maxc = g_RehldsSvs->GetMaxClients();
	int idx = userIdx - 1; // convert from 1-based status index to 0-based rehlds index
	if (idx < 0 || idx >= maxc) {
		SERVER_PRINT("[voice_export] Invalid player index\n");
		return;
	}
	IGameClient *cl = g_RehldsSvs->GetClient(idx);
	if (!cl || !cl->IsConnected()) {
		SERVER_PRINT("[voice_export] Player not connected\n");
		return;
	}
	g_playerVoiceBuffers[cl->GetId()].clear();
	SERVER_PRINT("[voice_export] Started segment\n");
}

static void Cmd_VoiceSegmentStop(void)
{
	if (!g_RehldsSvs) {
		SERVER_PRINT("[voice_export] ReHLDS not available\n");
		return;
	}
	if (CMD_ARGC() < 2) {
		SERVER_PRINT("Usage: voice_segment_stop <player_index>\n");
		return;
	}
	int userIdx = atoi(CMD_ARGV(1));
	int maxc = g_RehldsSvs->GetMaxClients();
	int idx = userIdx - 1;
	if (idx < 0 || idx >= maxc) {
		SERVER_PRINT("[voice_export] Invalid player index\n");
		return;
	}
	IGameClient *cl = g_RehldsSvs->GetClient(idx);
	if (!cl || !cl->IsConnected()) {
		SERVER_PRINT("[voice_export] Player not connected\n");
		return;
	}

	int id = cl->GetId();
	auto it = g_playerVoiceBuffers.find(id);
	if (it == g_playerVoiceBuffers.end() || it->second.empty()) {
		SERVER_PRINT("[voice_export] No data\n");
		return;
	}

	// Build file paths
	std::string speexPath = build_output_path(cl, ".speex");
	std::string jsonPath  = build_output_path(cl, ".json");

	// Ensure directories
	size_t lastSlash = speexPath.find_last_of('/');
	if (lastSlash != std::string::npos) {
		ensure_directory_chain(speexPath.substr(0, lastSlash));
	}

	// Write audio
	{
		// write to temporary raw path first
		std::string rawPath = speexPath + ".raw";
		FILE *f = fopen(rawPath.c_str(), "wb");
		if (!f) {
			SERVER_PRINT("[voice_export] Failed to open audio file\n");
			return;
		}
		fwrite(it->second.data(), 1, it->second.size(), f);
		fclose(f);

#ifndef _WIN32
		// Container outputs based on vx_codec: auto|speex|opus|both
		const char *mode = g_engfuncs.pfnCVarGetString ? g_engfuncs.pfnCVarGetString("vx_codec") : "auto";
		auto stIt2 = g_playerVoiceState.find(id);
		if (stIt2 != g_playerVoiceState.end()) {
			bool wantSpeex = true, wantOpus = false;
			if (mode) {
				if (!Q_stricmp(mode, "speex")) { wantSpeex = true; wantOpus = false; }
				else if (!Q_stricmp(mode, "opus")) { wantSpeex = false; wantOpus = true; }
				else if (!Q_stricmp(mode, "both")) { wantSpeex = true; wantOpus = true; }
				else { wantSpeex = true; wantOpus = true; } // auto -> both
			}
			if (wantSpeex) {
				int rate = (int)CVAR_GET_FLOAT("vx_rate"); if (rate <= 0) rate = 11025;
				std::string spxFinal = speexPath;
				if (spxFinal.size() >= 6 && spxFinal.rfind(".speex") == spxFinal.size() - 6) spxFinal.replace(spxFinal.size() - 6, 6, ".spx"); else spxFinal += ".spx";
				if (write_ogg_speex(stIt2->second.packets, spxFinal, rate)) {
					char info2[512]; std::snprintf(info2, sizeof(info2), "[voice_export] Wrote .spx: %s\n", spxFinal.c_str()); SERVER_PRINT(info2);
				}
			}
			if (wantOpus) {
				int orate = (int)CVAR_GET_FLOAT("vx_opus_rate"); if (orate <= 0) orate = 48000;
				std::string opusOut = speexPath; // reuse base
				if (opusOut.size() >= 6 && opusOut.rfind(".speex") == opusOut.size() - 6) opusOut.replace(opusOut.size() - 6, 6, ".opus"); else opusOut += ".opus";
				if (write_ogg_opus(stIt2->second.packets, opusOut, orate)) {
					char info3[512]; std::snprintf(info3, sizeof(info3), "[voice_export] Wrote .opus: %s\n", opusOut.c_str()); SERVER_PRINT(info3);
				}
			}
		}
#endif
	}

	// Metadata
	const char *name = cl->GetName();
	edict_t *pEdict = cl->GetEdict();
	const char *auth = GETPLAYERAUTHID(pEdict);
	INetChan *chan = cl->GetNetChan();
	const netadr_t *remote = chan ? chan->GetRemoteAdr() : nullptr;
	std::string ip = netadr_to_string(remote);

	{
		FILE *f = fopen(jsonPath.c_str(), "wb");
		if (f) {
			fprintf(f, "{\n");
			fprintf(f, "  \"steamid\": \"%s\",\n", auth ? auth : "");
			fprintf(f, "  \"name\": \"%s\",\n", name ? name : "");
			fprintf(f, "  \"ip\": \"%s\",\n", ip.c_str());
			fprintf(f, "  \"bytes\": %u\n", (unsigned)it->second.size());
			fprintf(f, "}\n");
			fclose(f);
		}
	}

	it->second.clear();
	char info[512];
	std::snprintf(info, sizeof(info), "[voice_export] Segment saved: %u bytes\n", (unsigned)it->second.size());
	SERVER_PRINT(info);

	// Reset state
	auto stIt = g_playerVoiceState.find(id);
	if (stIt != g_playerVoiceState.end()) {
		stIt->second.isRecording = false;
		stIt->second.segmentStartTime = 0.0;
		stIt->second.packets.clear();
	}
}

void VoiceCapture_RegisterServerCommands()
{
	g_engfuncs.pfnAddServerCommand("voice_segment_start", Cmd_VoiceSegmentStart);
	g_engfuncs.pfnAddServerCommand("voice_segment_stop", Cmd_VoiceSegmentStop);
}

// Periodic flush based on silence timeout
void VoiceCapture_OnStartFrame()
{
	if (!g_RehldsSvs || !g_RehldsApi)
		return;

	IRehldsServerData *svd = g_RehldsApi->GetServerData();
	double now = svd ? svd->GetTime() : 0.0;

	int maxc = g_RehldsSvs->GetMaxClients();
	for (int idx = 0; idx < maxc; ++idx)
	{
		IGameClient *cl = g_RehldsSvs->GetClient(idx);
		if (!cl)
			continue;

		int id = cl->GetId();
		auto stIt = g_playerVoiceState.find(id);
		if (stIt == g_playerVoiceState.end())
			continue;

		auto &st = stIt->second;
		if (!st.isRecording)
			continue;

		// Close segment if silence for threshold or client disconnected
		bool disconnected = !cl->IsConnected();
		bool silentTimeout = (now > 0.0 && (now - st.lastVoiceTime) >= kSilenceCloseSeconds);
		if ((silentTimeout || disconnected))
		{
			auto bufIt = g_playerVoiceBuffers.find(id);
			if (bufIt != g_playerVoiceBuffers.end() && !bufIt->second.empty())
			{
				// Build and write files (reuse logic from stop command)
				std::string speexPath = build_output_path(cl, ".speex");
				std::string jsonPath  = build_output_path(cl, ".json");

				size_t lastSlash = speexPath.find_last_of('/');
				if (lastSlash != std::string::npos) {
					ensure_directory_chain(speexPath.substr(0, lastSlash));
				}

				// Write raw audio then optionally wrap to .spx
				std::string rawPath = speexPath + ".raw";
				FILE *f = fopen(rawPath.c_str(), "wb");
				if (f) {
					fwrite(bufIt->second.data(), 1, bufIt->second.size(), f);
					fclose(f);
#ifndef _WIN32
					if (CVAR_GET_FLOAT && CVAR_GET_FLOAT("vx_spx") >= 1.0f) {
						int rate = (int)CVAR_GET_FLOAT("vx_rate");
						if (rate <= 0) rate = 11025;
						std::string spxFinal = speexPath;
						if (spxFinal.size() >= 6 && spxFinal.rfind(".speex") == spxFinal.size() - 6) {
							spxFinal.replace(spxFinal.size() - 6, 6, ".spx");
						} else {
							spxFinal += ".spx";
						}
						char cmd[1024];
						std::snprintf(cmd, sizeof(cmd), "speexenc --quiet --rate %d --quality 5 '%s' '%s' 2>/dev/null", rate, rawPath.c_str(), spxFinal.c_str());
						int rc = system(cmd);
						if (rc == 0) {
							remove(rawPath.c_str());
							char info2[512];
							std::snprintf(info2, sizeof(info2), "[voice_export] Wrapped to Ogg Speex: %s\n", spxFinal.c_str());
							SERVER_PRINT(info2);
						} else {
							char info2[512];
							std::snprintf(info2, sizeof(info2), "[voice_export] speexenc not available or failed (rc=%d). Kept raw: %s\n", rc, rawPath.c_str());
							SERVER_PRINT(info2);
						}
					}
#endif
				}

				// Metadata
				const char *name = cl->GetName();
				edict_t *pEdict = cl->GetEdict();
				const char *auth = GETPLAYERAUTHID(pEdict);
				INetChan *chan = cl->GetNetChan();
				const netadr_t *remote = chan ? chan->GetRemoteAdr() : nullptr;
				std::string ip = netadr_to_string(remote);

				FILE *fj = fopen(jsonPath.c_str(), "wb");
				if (fj) {
					fprintf(fj, "{\n");
					fprintf(fj, "  \"steamid\": \"%s\",\n", auth ? auth : "");
					fprintf(fj, "  \"name\": \"%s\",\n", name ? name : "");
					fprintf(fj, "  \"ip\": \"%s\",\n", ip.c_str());
					fprintf(fj, "  \"bytes\": %u\n", (unsigned)bufIt->second.size());
					fprintf(fj, "}\n");
					fclose(fj);
				}

				char info[640];
				std::snprintf(info, sizeof(info), "[voice_export] REC STOP: slot=%d name=\"%s\" %u bytes -> %s\n", idx + 1, name ? name : "", (unsigned)bufIt->second.size(), speexPath.c_str());
				SERVER_PRINT(info);

				bufIt->second.clear();
			}

			st.isRecording = false;
			st.segmentStartTime = 0.0;
			st.packets.clear();
		}
	}
}


