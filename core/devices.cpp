#include "devices.hpp"

#include <chrono>
#include <cstdio>

#ifdef _WIN32
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#endif

namespace myobs {

#ifdef _WIN32
namespace {

std::string Utf8(const wchar_t *w)
{
	if (!w)
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring Wide(const std::string &s)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1)
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}

template<class T> void Release(T *&p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

struct ComScope {
	HRESULT hr;
	ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
	~ComScope()
	{
		if (SUCCEEDED(hr))
			CoUninitialize();
	}
};

} // namespace

std::vector<AudioDeviceInfo> ListOutputDevices()
{
	std::vector<AudioDeviceInfo> out;
	ComScope com;
	IMMDeviceEnumerator *en = nullptr;
	IMMDeviceCollection *list = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				    (void **)&en)))
		return out;
	if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) {
		UINT count = 0;
		list->GetCount(&count);
		for (UINT i = 0; i < count; i++) {
			IMMDevice *dev = nullptr;
			if (FAILED(list->Item(i, &dev)))
				continue;
			LPWSTR wid = nullptr;
			IPropertyStore *props = nullptr;
			AudioDeviceInfo info;
			if (SUCCEEDED(dev->GetId(&wid))) {
				info.id = Utf8(wid);
				CoTaskMemFree(wid);
			}
			if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
				PROPVARIANT pv;
				PropVariantInit(&pv);
				if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
					info.name = Utf8(pv.pwszVal);
				PropVariantClear(&pv);
				Release(props);
			}
			if (!info.id.empty())
				out.push_back(info);
			Release(dev);
		}
		Release(list);
	}
	Release(en);
	return out;
}

bool DeviceSink::PlayOnce()
{
	IMMDeviceEnumerator *en = nullptr;
	IMMDevice *dev = nullptr;
	IAudioClient *client = nullptr;
	IAudioRenderClient *render = nullptr;
	HANDLE event = nullptr;
	bool ok = false;
	auto fail = [&](const char *what, HRESULT hr) {
		std::lock_guard<std::mutex> lock(errMutex_);
		char buf[128];
		snprintf(buf, sizeof buf, "%s (0x%08lx)", what, (unsigned long)hr);
		error_ = buf;
	};
	do {
		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
					      __uuidof(IMMDeviceEnumerator), (void **)&en);
		if (FAILED(hr)) {
			fail("device enumerator", hr);
			break;
		}
		hr = en->GetDevice(Wide(id_).c_str(), &dev);
		if (FAILED(hr)) {
			fail("device not found", hr);
			break;
		}
		hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);
		if (FAILED(hr)) {
			fail("activate", hr);
			break;
		}
		WAVEFORMATEXTENSIBLE wf = {};
		wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wf.Format.nChannels = 2;
		wf.Format.nSamplesPerSec = 48000;
		wf.Format.wBitsPerSample = 32;
		wf.Format.nBlockAlign = 8;
		wf.Format.nAvgBytesPerSec = 48000 * 8;
		wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		wf.Samples.wValidBitsPerSample = 32;
		wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
		wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
		const REFERENCE_TIME period = 100000; /* 10 ms */
		hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
					AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
						AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
					period, 0, (WAVEFORMATEX *)&wf, nullptr);
		if (FAILED(hr)) {
			fail("initialize", hr);
			break;
		}
		event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		client->SetEventHandle(event);
		UINT32 frames = 0;
		client->GetBufferSize(&frames);
		hr = client->GetService(__uuidof(IAudioRenderClient), (void **)&render);
		if (FAILED(hr)) {
			fail("render client", hr);
			break;
		}
		hr = client->Start();
		if (FAILED(hr)) {
			fail("start", hr);
			break;
		}
		{
			std::lock_guard<std::mutex> lock(errMutex_);
			error_.clear();
		}
		running_ = true;
		ok = true;
		while (!stop_) {
			WaitForSingleObject(event, 100);
			UINT32 padding = 0;
			hr = client->GetCurrentPadding(&padding);
			if (FAILED(hr)) {
				fail("device lost", hr);
				ok = false;
				break;
			}
			const UINT32 avail = frames - padding;
			if (!avail)
				continue;
			BYTE *data = nullptr;
			hr = render->GetBuffer(avail, &data);
			if (FAILED(hr)) {
				fail("device lost", hr);
				ok = false;
				break;
			}
			buf_.Pull(reinterpret_cast<float *>(data), avail);
			render->ReleaseBuffer(avail, 0);
		}
		client->Stop();
	} while (false);
	running_ = false;
	Release(render);
	Release(client);
	Release(dev);
	Release(en);
	if (event)
		CloseHandle(event);
	return ok;
}

void DeviceSink::Run()
{
	ComScope com;
	while (!stop_) {
		PlayOnce();
		for (int i = 0; i < 20 && !stop_; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

#else

std::vector<AudioDeviceInfo> ListOutputDevices()
{
	return {};
}

bool DeviceSink::PlayOnce()
{
	return false;
}

void DeviceSink::Run()
{
	std::lock_guard<std::mutex> lock(errMutex_);
	error_ = "audio devices are supported on Windows only";
}

#endif

DeviceSink::DeviceSink(std::string deviceId, double delayMs) : id_(std::move(deviceId)), buf_(delayMs, 48000)
{
	thread_ = std::thread([this]() { Run(); });
}

DeviceSink::~DeviceSink()
{
	stop_ = true;
	if (thread_.joinable())
		thread_.join();
}

std::string DeviceSink::Error() const
{
	std::lock_guard<std::mutex> lock(errMutex_);
	return error_;
}

} // namespace myobs
