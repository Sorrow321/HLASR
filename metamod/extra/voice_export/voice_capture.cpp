#include "voice_capture.h"
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <algorithm>
#include <sstream>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif
#include "enginecallbacks.h"

static std::unordered_map<int, std::vector<unsigned char>> g_playerVoiceBuffers;

extern "C" {
	extern cvar_t vx_debug;
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

// Hook: HandleNetCommand
static void OnHandleNetCommand(IVoidHookChain<IGameClient*, int8>* chain, IGameClient* client, int8 cmd)
{
	if (!g_RehldsFuncs)
		return chain->callNext(client, cmd);

	sizebuf_t *msg = g_RehldsFuncs->GetNetMessage();
	int *pReadCount = g_RehldsFuncs->GetMsgReadCount();
	int beforeRead = pReadCount ? *pReadCount : 0;
	double beforeVoice = client->GetLastVoiceTime();

	chain->callNext(client, cmd);

	double afterVoice = client->GetLastVoiceTime();
	int afterRead = pReadCount ? *pReadCount : beforeRead;

	if (afterVoice > beforeVoice && msg && afterRead > beforeRead && msg->data && afterRead <= msg->cursize) {
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
		g_RehldsHookchains->HandleNetCommand()->registerHook(OnHandleNetCommand);
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
		// Optionally wrap to Ogg Speex via speexenc if available
		if (CVAR_GET_FLOAT && CVAR_GET_FLOAT("vx_spx") >= 1.0f) {
			int rate = (int)CVAR_GET_FLOAT("vx_rate");
			if (rate <= 0) rate = 11025;
			std::string spxOut = speexPath; // final .speex path requested to change to .spx later
			// We will actually output .spx
			std::string spxFinal = spxOut;
			// replace extension .speex with .spx
			if (spxFinal.size() >= 6 && spxFinal.rfind(".speex") == spxFinal.size() - 6) {
				spxFinal.replace(spxFinal.size() - 6, 6, ".spx");
			} else {
				spxFinal += ".spx";
			}
			char cmd[1024];
			std::snprintf(cmd, sizeof(cmd), "speexenc --quiet --rate %d --quality 5 '%s' '%s' 2>/dev/null", rate, rawPath.c_str(), spxFinal.c_str());
			int rc = system(cmd);
			if (rc == 0) {
				// remove raw and report
				remove(rawPath.c_str());
				char info[512];
				std::snprintf(info, sizeof(info), "[voice_export] Wrapped to Ogg Speex: %s\n", spxFinal.c_str());
				SERVER_PRINT(info);
			} else {
				char info[512];
				std::snprintf(info, sizeof(info), "[voice_export] speexenc not available or failed (rc=%d). Kept raw: %s\n", rc, rawPath.c_str());
				SERVER_PRINT(info);
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
		}
	}
}


