#include <extdll.h>
#include <meta_api.h>
#include "vx_rehlds_api.h"
#include "voice_capture.h"
#include <string.h>

meta_globals_t *gpMetaGlobals;
gamedll_funcs_t *gpGamedllFuncs;
mutil_funcs_t *gpMetaUtilFuncs;
enginefuncs_t *g_pengfuncsTable;

plugin_info_t Plugin_info =
{
	META_INTERFACE_VERSION,
	"Voice Export",
	"0.1",
	__DATE__,
	"HLASR",
	"http://",
	"VEXPORT",
	PT_ANYTIME,
	PT_ANYTIME,
};

C_DLLEXPORT int Meta_Query(char *interfaceVersion, plugin_info_t **plinfo, mutil_funcs_t *pMetaUtilFuncs)
{
	*plinfo = &Plugin_info;
	gpMetaUtilFuncs = pMetaUtilFuncs;
	return TRUE;
}

META_FUNCTIONS gMetaFunctionTable =
{
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	GetEngineFunctions,
	GetEngineFunctions_Post,
};

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME now, META_FUNCTIONS *pFunctionTable, meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs)
{
	gpMetaGlobals = pMGlobals;
	gpGamedllFuncs = pGamedllFuncs;

	if (meta_init_rehlds_api()) {
		g_engfuncs.pfnServerPrint("[voice_export] ReHLDS API initialized.\n");
		VoiceCapture_RegisterHooks();
	} else {
		g_engfuncs.pfnServerPrint("[voice_export] ReHLDS API not available; plugin will be idle.\n");
	}

	VoiceCapture_RegisterServerCommands();

	memcpy(pFunctionTable, &gMetaFunctionTable, sizeof(META_FUNCTIONS));
	return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON reason)
{
	VoiceCapture_UnregisterHooks();
	return TRUE;
}

// Minimal engine function tables to receive StartFrame callbacks
static enginefuncs_t g_EngineFunctionsTable =
{
	/* 0-45 */ 0
};

static enginefuncs_t g_EngineFunctionsTable_Post =
{
	/* 0-45 */ 0
};

C_DLLEXPORT int GetEngineFunctions(enginefuncs_t *pengfuncsFromEngine, int *interfaceVersion)
{
	if (!pengfuncsFromEngine || !interfaceVersion)
		return FALSE;

	// Fill only StartFrame; leave others null
	memset(&g_EngineFunctionsTable, 0, sizeof(g_EngineFunctionsTable));
	g_EngineFunctionsTable.pfnStartFrame = []() { VoiceCapture_OnStartFrame(); };

	memcpy(pengfuncsFromEngine, &g_EngineFunctionsTable, sizeof(enginefuncs_t));
	return TRUE;
}

C_DLLEXPORT int GetEngineFunctions_Post(enginefuncs_t *pengfuncsFromEngine, int *interfaceVersion)
{
	if (!pengfuncsFromEngine || !interfaceVersion)
		return FALSE;

	memset(&g_EngineFunctionsTable_Post, 0, sizeof(g_EngineFunctionsTable_Post));
	// No post callback needed currently

	memcpy(pengfuncsFromEngine, &g_EngineFunctionsTable_Post, sizeof(enginefuncs_t));
	return TRUE;
}


