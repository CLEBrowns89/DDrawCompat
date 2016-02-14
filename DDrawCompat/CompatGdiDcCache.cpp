#include <vector>

#include "CompatDirectDraw.h"
#include "CompatDirectDrawSurface.h"
#include "CompatGdiDcCache.h"
#include "CompatPrimarySurface.h"
#include "Config.h"
#include "DDrawLog.h"
#include "DDrawProcs.h"

namespace
{
	using CompatGdiDcCache::CachedDc;
	
	std::vector<CachedDc> g_cache;
	DWORD g_cacheSize = 0;
	DWORD g_cacheId = 0;
	DWORD g_maxUsedCacheSize = 0;
	DWORD g_ddLockThreadId = 0;

	IDirectDraw7* g_directDraw = nullptr;
	void* g_surfaceMemory = nullptr;
	LONG g_pitch = 0;

	IDirectDrawSurface7* createGdiSurface();

	IDirectDraw7* createDirectDraw()
	{
		IDirectDraw7* dd = nullptr;
		CALL_ORIG_DDRAW(DirectDrawCreateEx, nullptr, reinterpret_cast<LPVOID*>(&dd), IID_IDirectDraw7, nullptr);
		if (!dd)
		{
			Compat::Log() << "Failed to create a DirectDraw interface for GDI";
			return nullptr;
		}

		if (FAILED(CompatDirectDraw<IDirectDraw7>::s_origVtable.SetCooperativeLevel(dd, nullptr, DDSCL_NORMAL)))
		{
			Compat::Log() << "Failed to set the cooperative level on the DirectDraw interface for GDI";
			dd->lpVtbl->Release(dd);
			return nullptr;
		}

		return dd;
	}

	CachedDc createCachedDc()
	{
		CachedDc cachedDc = {};

		IDirectDrawSurface7* surface = createGdiSurface();
		if (!surface)
		{
			return cachedDc;
		}

		HDC dc = nullptr;
		HRESULT result = surface->lpVtbl->GetDC(surface, &dc);
		if (FAILED(result))
		{
			LOG_ONCE("Failed to create a GDI DC: " << result);
			surface->lpVtbl->Release(surface);
			return cachedDc;
		}

		// Release DD critical section acquired by IDirectDrawSurface7::GetDC to avoid deadlocks
		Compat::origProcs.ReleaseDDThreadLock();

		cachedDc.surface = surface;
		cachedDc.dc = dc;
		cachedDc.cacheId = g_cacheId;
		return cachedDc;
	}

	IDirectDrawSurface7* createGdiSurface()
	{
		DDSURFACEDESC2 desc = {};
		desc.dwSize = sizeof(desc);
		desc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_CAPS | DDSD_PITCH | DDSD_LPSURFACE;
		desc.dwWidth = CompatPrimarySurface::width;
		desc.dwHeight = CompatPrimarySurface::height;
		desc.ddpfPixelFormat = CompatPrimarySurface::pixelFormat;
		desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
		desc.lPitch = g_pitch;
		desc.lpSurface = g_surfaceMemory;

		IDirectDrawSurface7* surface = nullptr;
		HRESULT result = CompatDirectDraw<IDirectDraw7>::s_origVtable.CreateSurface(
			g_directDraw, &desc, &surface, nullptr);
		if (FAILED(result))
		{
			LOG_ONCE("Failed to create a GDI surface: " << result);
			return nullptr;
		}

		if (CompatPrimarySurface::palette)
		{
			CompatDirectDrawSurface<IDirectDrawSurface7>::s_origVtable.SetPalette(
				surface, CompatPrimarySurface::palette);
		}

		return surface;
	}

	void extendCache()
	{
		if (g_cacheSize >= Config::preallocatedGdiDcCount)
		{
			LOG_ONCE("Warning: Preallocated GDI DC count is insufficient. This may lead to graphical issues.");
		}

		if (GetCurrentThreadId() != g_ddLockThreadId)
		{
			return;
		}

		for (DWORD i = 0; i < Config::preallocatedGdiDcCount; ++i)
		{
			CachedDc cachedDc = createCachedDc();
			if (!cachedDc.dc)
			{
				return;
			}
			g_cache.push_back(cachedDc);
			++g_cacheSize;
		}
	}

	void releaseCachedDc(CachedDc cachedDc)
	{
		// Reacquire DD critical section that was temporarily released after IDirectDrawSurface7::GetDC
		Compat::origProcs.AcquireDDThreadLock();

		HRESULT result = CompatDirectDrawSurface<IDirectDrawSurface7>::s_origVtable.ReleaseDC(
			cachedDc.surface, cachedDc.dc);
		if (FAILED(result))
		{
			LOG_ONCE("Failed to release a cached DC: " << result);
		}

		CompatDirectDrawSurface<IDirectDrawSurface7>::s_origVtable.Release(cachedDc.surface);
	}
}

namespace CompatGdiDcCache
{
	void clear()
	{
		for (auto& cachedDc : g_cache)
		{
			releaseCachedDc(cachedDc);
		}
		g_cache.clear();
		g_cacheSize = 0;
		++g_cacheId;
	}

	CachedDc getDc()
	{
		CachedDc cachedDc = {};
		if (!g_surfaceMemory)
		{
			return cachedDc;
		}

		if (g_cache.empty())
		{
			extendCache();
		}

		if (!g_cache.empty())
		{
			cachedDc = g_cache.back();
			g_cache.pop_back();

			const DWORD usedCacheSize = g_cacheSize - g_cache.size();
			if (usedCacheSize > g_maxUsedCacheSize)
			{
				g_maxUsedCacheSize = usedCacheSize;
				Compat::Log() << "GDI used DC cache size: " << g_maxUsedCacheSize;
			}
		}

		return cachedDc;
	}

	bool init()
	{
		g_directDraw = createDirectDraw();
		return nullptr != g_directDraw;
	}

	void releaseDc(const CachedDc& cachedDc)
	{
		if (cachedDc.cacheId == g_cacheId)
		{
			g_cache.push_back(cachedDc);
		}
		else
		{
			releaseCachedDc(cachedDc);
		}
	}

	void setDdLockThreadId(DWORD ddLockThreadId)
	{
		g_ddLockThreadId = ddLockThreadId;
	}

	void setSurfaceMemory(void* surfaceMemory, LONG pitch)
	{
		if (g_surfaceMemory == surfaceMemory && g_pitch == pitch)
		{
			return;
		}

		g_surfaceMemory = surfaceMemory;
		g_pitch = pitch;
		clear();
	}
}
