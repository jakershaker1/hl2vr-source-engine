/*
Copyright (C) 2022 nillerusr

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of 
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL_hints.h>
#include "tier0/dbg.h"
#include "tier0/threadtools.h"

char *LauncherArgv[512];
char java_args[4096];
int iLastArgs = 0;

extern void InitCrashHandler();
DLL_EXPORT int LauncherMain( int argc, char **argv ); // from launcher.cpp

DLL_EXPORT int Java_com_valvesoftware_ValveActivity2_setenv(JNIEnv *jenv, jclass *jclass, jstring env, jstring value, jint over)
{
	Msg( "Java_com_valvesoftware_ValveActivity2_setenv %s=%s\n", jenv->GetStringUTFChars(env, NULL), jenv->GetStringUTFChars(value, NULL) );
	return setenv( jenv->GetStringUTFChars(env, NULL), jenv->GetStringUTFChars(value, NULL), over );
}

DLL_EXPORT void Java_com_valvesoftware_ValveActivity2_nativeOnActivityResult()
{
//	Msg( "Java_com_valvesoftware_ValveActivity_nativeOnActivityResult\n" );
}

DLL_EXPORT void Java_com_valvesoftware_ValveActivity2_setArgs(JNIEnv *env, jclass *clazz, jstring str)
{
	strncpy( java_args, env->GetStringUTFChars(str, NULL), sizeof java_args );
}

void SetLauncherArgs()
{
#define A(a,b) LauncherArgv[iLastArgs++] = (char*)a; \
	LauncherArgv[iLastArgs++] = (char*)b
#define D(a) LauncherArgv[iLastArgs++] = (char*)a

	static char binPath[2048];
	snprintf(binPath, sizeof binPath, "%s/hl2_linux", getenv("APP_DATA_PATH") );
	D(binPath);

	D("-nouserclip");

	char *pch;

	pch = strtok (java_args," ");
	while (pch != NULL)
	{
		LauncherArgv[iLastArgs++] = pch;
		pch = strtok (NULL, " ");
	}

	D("-fullscreen");
	D("-nosteam");
	D("-insecure");

	// NOTE: no -vr switch here. The engine-side VR paths are enabled directly
	// in CEngineAPI (engine/sys_dll2.cpp) on Android instead - this build is
	// VR-only, and tier0's command-line builder mangles the tail of the
	// argument list on this platform, so a flag here would not reliably
	// survive to CheckParm anyway.

	// NOTE: deliberately no -w/-h here. They'd only feed FindVideoMode(),
	// which snaps to an enumerated video mode, and Android has none to
	// enumerate - so the stereo resolution is forced further down the chain
	// instead (engine/sys_getmodes.cpp and shaderapidx9/shaderdevicedx8.cpp).

	// Both of the engine's own VR HUD paths stay off. The UI is instead drawn
	// into its own strip of the backbuffer and composited by the runtime as an
	// OpenXR quad layer - see launcher/android/vr_xr_gles.cpp.
	//
	// vr_render_hud_in_world=1 would select RenderHUDQuad
	// (client_virtualreality.cpp), which draws a textured quad in the scene.
	// That path is broken here: it renders as a missing-texture checkerboard
	// even when pointed at an ordinary disk texture that loads fine, so the
	// fault is in its draw path (dynamic mesh + UnlitGeneric under togles),
	// not the texture or the render target. It is 2013 OpenVR-era code that
	// has been dormant for a decade; the quad layer replaces it rather than
	// repairing it, and needs no material, mesh or shader at all.
	//
	// vr_hud_never_overlay=1 keeps the engine off the overlay/composite path
	// too, since our ISourceVirtualReality::CompositeHud is a no-op.
	A("+vr_hud_never_overlay", "1");
	A("+vr_render_hud_in_world", "0");

	// NOTE: touch controls are disabled by changing TOUCH_DEFAULT in
	// game/client/touch.cpp rather than with a convar here - see the note
	// above about the tail of this argument list getting mangled.

#undef A
#undef D
}

float GetTotalMemory()
{
	int64_t mem = 0;

	char meminfo[8196] = { 0 };
	FILE *f = fopen("/proc/meminfo", "r");
	if( !f )
		return 0.f;

	size_t size = fread(meminfo, 1, sizeof(meminfo), f);
	if( !size )
		return 0.f;

	char *s = strstr(meminfo, "MemTotal:");

	if( !s ) return 0.f;

	sscanf(s+9, "%lld", &mem);
	fclose(f);

	return mem/1024/1024.f;
}

void android_property_print(const char *name)
{
	char prop[1024];

	char strValue[64];
	memset (strValue, 0, 64);
	snprintf(prop, sizeof(prop), "getprop %s", name);
	FILE *fp = NULL;
	fp = popen(prop, "r");
	if (!fp) return;

	fgets(strValue, sizeof(strValue), fp);
	pclose(fp);
	fp = NULL;

	Msg("prop %s=%s", name, strValue);
}


DLL_EXPORT int LauncherMainAndroid( int argc, char **argv )
{
	InitCrashHandler();

	Msg("GetTotalMemory() = %.2f \n", GetTotalMemory());

	android_property_print("ro.build.version.sdk");
	android_property_print("ro.product.device");
	android_property_print("ro.product.manufacturer");
	android_property_print("ro.product.model");
	android_property_print("ro.product.name");

	SetLauncherArgs();

	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	DeclareCurrentThreadIsMainThread(); // Init thread propertly on Android

	return LauncherMain(iLastArgs, LauncherArgv);
}
