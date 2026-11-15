/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Dan B. Cooper */
/*
 * egl_headless.c - an OpenGL ES 3 context without a window (EGL, Mesa "surfaceless" platform) for the headless test shell. On a machine without a GPU
 * Mesa's llvmpipe draws it on the CPU. Only used with --gpu (main_headless.c).
 */
#include <stdio.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "../core/gpu_gles.h"

static EGLDisplay sDpy;
static EGLSurface sSurf;
static EGLContext sCtx;

static int make_current(void *user)
{
    (void)user;
    return eglMakeCurrent(sDpy, sSurf, sSurf, sCtx) ? 0 : -1;
}

static void release(void *user)
{
    (void)user;
    eglMakeCurrent(sDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

/* Creates the context (not current in the calling thread) and starts the back end. Returns 0 on success. */
int egl_headless_start(void)
{
    static GlesHost host;
    EGLint maj, min, n = 0;
    EGLConfig cfg;
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    sDpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(sDpy, &maj, &min)) { fprintf(stderr, "egl: initialise failed (0x%X)\n", eglGetError()); return -1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(sDpy, ca, &cfg, 1, &n) || !n) { fprintf(stderr, "egl: no ES3 configuration\n"); return -1; }
    sSurf = eglCreatePbufferSurface(sDpy, cfg, pa);
    sCtx = eglCreateContext(sDpy, cfg, EGL_NO_CONTEXT, xa);
    if (sSurf == EGL_NO_SURFACE || sCtx == EGL_NO_CONTEXT) { fprintf(stderr, "egl: context creation failed (0x%X)\n", eglGetError()); return -1; }
    host.make_current = make_current;
    host.release = release;
    return gles_start(&host);
}
