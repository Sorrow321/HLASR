#include <extdll.h>
#include <meta_api.h>
#include "vx_rehlds_api.h"
#include "voice_capture.h"
#include <string.h>

extern "C" {
	cvar_t vx_debug = {
		"vx_debug",
		"1",
		FCVAR_EXTDLL,
		0,
		nullptr
	};
}

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
	GetEntityAPI2,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME now, META_FUNCTIONS *pFunctionTable, meta_globals_t *pMGlobals, gamedll_funcs_t *pGamedllFuncs)
{
	gpMetaGlobals = pMGlobals;
	gpGamedllFuncs = pGamedllFuncs;

	// Register debug cvar
	CVAR_REGISTER(&vx_debug);

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

// Hook StartFrame via DLL API (game callbacks)
static void VX_StartFrame(void)
{
	VoiceCapture_OnStartFrame();
}

C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS *pFunctionTable, int *interfaceVersion)
{
	if (!pFunctionTable || !interfaceVersion)
		return FALSE;
	memset(pFunctionTable, 0, sizeof(DLL_FUNCTIONS));
	pFunctionTable->pfnStartFrame = VX_StartFrame;
	return TRUE;
}


