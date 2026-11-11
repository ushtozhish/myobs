/* MyOBS: choose the render GPU by identity, never by position (header-only, Windows).
 *
 * libobs renders on DXGI adapter number N, and Windows gives number 0 to the GPU that
 * drives the primary monitor - with the primary monitor on the motherboard, that is the
 * integrated GPU. MyOBS always renders on a discrete GPU:
 *   1. the saved render GPU name, if that adapter exists and is discrete;
 *   2. otherwise the discrete adapter with the most dedicated video memory, NVIDIA first;
 *   3. only if no discrete adapter exists: the best remaining one, with a warning.
 * Monitors on an integrated GPU stay usable (Windows copies the picture across).
 * dxgi.dll is loaded at run time: no extra link libraries.
 */
#pragma once
#ifdef _WIN32

#include <windows.h>
#include <dxgi.h>
#include <util/base.h>
#include <string>
#include <vector>

namespace MyOBSGpu {

struct Adapter {
	uint32_t index = 0;
	std::wstring name;
	std::string nameUtf8;
	UINT vendor = 0;
	SIZE_T dedicated = 0;
	LUID luid = {};
	bool software = false;
	bool integrated = false;
	std::vector<std::string> monitors;
};

static inline std::string ToUtf8(const std::wstring &w)
{
	if (w.empty())
		return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

static inline std::vector<Adapter> Enumerate()
{
	std::vector<Adapter> list;
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	if (!dxgi)
		return list;
	using CreateFactory1 = HRESULT(WINAPI *)(REFIID, void **);
	auto create = reinterpret_cast<CreateFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
	IDXGIFactory1 *factory = nullptr;
	if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory) {
		FreeLibrary(dxgi);
		return list;
	}
	for (UINT i = 0;; i++) {
		IDXGIAdapter1 *adapter = nullptr;
		if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter)
			break;
		DXGI_ADAPTER_DESC1 desc = {};
		if (SUCCEEDED(adapter->GetDesc1(&desc))) {
			Adapter a;
			a.index = i;
			a.name = desc.Description;
			a.nameUtf8 = ToUtf8(a.name);
			a.vendor = desc.VendorId;
			a.dedicated = desc.DedicatedVideoMemory;
			a.luid = desc.AdapterLuid;
			/* Microsoft Basic Render Driver / WARP */
			a.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || desc.VendorId == 0x1414;
			/* Integrated GPUs share system memory and report little or no dedicated VRAM.
			 * Intel Arc and other discrete cards report gigabytes, so they stay discrete. */
			a.integrated = !a.software && desc.DedicatedVideoMemory < (SIZE_T)512 * 1024 * 1024;
			for (UINT o = 0;; o++) {
				IDXGIOutput *output = nullptr;
				if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND || !output)
					break;
				DXGI_OUTPUT_DESC od = {};
				if (SUCCEEDED(output->GetDesc(&od))) {
					const RECT &r = od.DesktopCoordinates;
					a.monitors.push_back(ToUtf8(od.DeviceName) + " " + std::to_string(r.right - r.left) +
							     "x" + std::to_string(r.bottom - r.top));
				}
				output->Release();
			}
			list.push_back(a);
		}
		adapter->Release();
	}
	factory->Release();
	FreeLibrary(dxgi);
	return list;
}

/* Ask Windows to give this program the high-performance GPU (same as Settings > Display >
 * Graphics > High performance). Takes effect from the next start; harmless if ignored. */
static inline void RequestHighPerformanceGpu()
{
	wchar_t exe[MAX_PATH] = {};
	DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
	if (len == 0 || len >= MAX_PATH)
		return;
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0,
			    KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
		return;
	const wchar_t want[] = L"GpuPreference=2;";
	wchar_t have[64] = {};
	DWORD type = 0, size = sizeof(have) - sizeof(wchar_t);
	bool same = RegQueryValueExW(key, exe, nullptr, &type, reinterpret_cast<BYTE *>(have), &size) == ERROR_SUCCESS &&
		    type == REG_SZ && wcscmp(have, want) == 0;
	if (!same)
		RegSetValueExW(key, exe, 0, REG_SZ, reinterpret_cast<const BYTE *>(want), sizeof(want));
	RegCloseKey(key);
}

static inline bool Better(const Adapter &a, const Adapter &b)
{
	bool aNv = a.vendor == 0x10DE, bNv = b.vendor == 0x10DE;
	if (aNv != bNv)
		return aNv;
	return a.dedicated > b.dedicated;
}

/* Returns the DXGI index libobs must use. "preferred" is the saved render GPU name (may be
 * empty); "chosenName" receives the name of the GPU actually chosen. */
static inline uint32_t ResolveRenderAdapter(const std::string &preferred, std::string &chosenName)
{
	static bool requested = false;
	if (!requested) {
		RequestHighPerformanceGpu();
		requested = true;
	}

	std::vector<Adapter> list = Enumerate();
	if (list.empty()) {
		blog(LOG_WARNING, "[MyOBS GPU] DXGI enumeration failed, using adapter 0");
		return 0;
	}

	for (const Adapter &a : list) {
		std::string mon;
		for (const std::string &m : a.monitors)
			mon += (mon.empty() ? "" : ", ") + m;
		blog(LOG_INFO, "[MyOBS GPU] #%u %s | vendor 0x%04X | VRAM %llu MB | %s | monitors: %s", a.index,
		     a.nameUtf8.c_str(), a.vendor, (unsigned long long)(a.dedicated / (1024 * 1024)),
		     a.software ? "software" : (a.integrated ? "integrated" : "discrete"),
		     mon.empty() ? "none" : mon.c_str());
	}

	const Adapter *chosen = nullptr;
	if (!preferred.empty()) {
		for (const Adapter &a : list)
			if (!a.software && !a.integrated && a.nameUtf8 == preferred) {
				chosen = &a;
				break;
			}
	}
	if (!chosen)
		for (const Adapter &a : list)
			if (!a.software && !a.integrated && (!chosen || Better(a, *chosen)))
				chosen = &a;
	if (!chosen) {
		/* No discrete GPU (or a machine without any GPU, like CI): best remaining one. */
		for (const Adapter &a : list)
			if (!chosen || a.dedicated > chosen->dedicated)
				chosen = &a;
		blog(LOG_WARNING, "[MyOBS GPU] NO DISCRETE GPU FOUND - rendering on '%s'", chosen->nameUtf8.c_str());
	}
	blog(LOG_INFO, "[MyOBS GPU] render GPU: #%u %s", chosen->index, chosen->nameUtf8.c_str());
	chosenName = chosen->nameUtf8;
	return chosen->index;
}

} // namespace MyOBSGpu

#endif
