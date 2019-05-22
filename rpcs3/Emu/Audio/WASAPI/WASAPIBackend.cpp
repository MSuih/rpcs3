#ifdef _WIN32

#include "WASAPIBackend.h"

#include "audioclient.h"
#include "combaseapi.h"
#include "comdef.h"
#include "devpkey.h"
#include "functiondiscoverykeys_devpkey.h"
#include "propidl.h"
#include "stringapiset.h"

extern cfg_root g_cfg;

bool check(const char *message, HRESULT result);

static const DWORD mask2Channel = 0x1 | 0x2;
static const DWORD mask8Channel = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x200 | 0x400;

WASAPIBackend::WASAPIBackend()
{
	m_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	m_format.Format.nChannels = get_channels();
	m_format.Format.nSamplesPerSec = 48000; // 48 KHz
	m_format.Format.wBitsPerSample = get_sample_size() * 8;
	m_format.Format.nBlockAlign = m_format.Format.nChannels * m_format.Format.wBitsPerSample / 8;
	m_format.Format.nAvgBytesPerSec = m_format.Format.nSamplesPerSec * m_format.Format.nBlockAlign;
	m_format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

	m_format.Samples.wValidBitsPerSample = get_sample_size();
	m_format.dwChannelMask = g_cfg.audio.downmix_to_2ch ? mask2Channel : mask8Channel;
	m_format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM; // ?

	check("Could not initialize COM library", CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY));
	check("Could not initialize enumerator",
		CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator),
			reinterpret_cast<LPVOID*>(&m_enumerator)));
}

WASAPIBackend::~WASAPIBackend()
{
	if (m_enumerator)
		m_enumerator->Release();
	CoUninitialize();
}

std::string wcharToStdString(LPCWCH input)
{
	int size = WideCharToMultiByte(CP_UTF8, NULL, input, -1, nullptr, 0, NULL, NULL);
	std::vector<char> buffer(size);
	verify(HERE), WideCharToMultiByte(CP_UTF8, NULL, input, -1, static_cast<LPSTR>(buffer.data()), size, NULL, NULL) != 0;
	std::string str(buffer.data());
	return str;
}

bool check(const char *message, HRESULT result)
{
	if (result != S_OK)
	{
		_com_error err(result);
		const TCHAR *msg = err.ErrorMessage();
		std::string str = wcharToStdString(static_cast<const LPCWCH>(msg));
		LOG_ERROR(GENERAL, "WASAPI: %s (Message: %s)", message, str.c_str());
		return false;
	}
	return true;
}

void WASAPIBackend::findPreferredDevice(IMMDevice **dev) {
	IMMDeviceCollection *devices = nullptr;
	if (!m_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))
		return;

	std::string preferredDevice = g_cfg.audio.preferred_audio_device;
	UINT count = 0;
	devices->GetCount(&count);

	bool found = false;
	for (UINT i = 0; !found && i < count; i++) {
		IMMDevice *device = nullptr;
		if ((devices->Item(i, &device) != S_OK))
			continue;

		IPropertyStore *store = nullptr;
		if (!check("Could not open properties for device", device->OpenPropertyStore(STGM_READ, &store)))
		{
			PROPVARIANT prop;
			PropVariantInit(&prop);

			if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &prop)))
			{
				std::string deviceName = wcharToStdString(prop.pwszVal);

				if (deviceName == preferredDevice)
				{
					found = true;
					*dev = device;
				}
			}

			PropVariantClear(&prop);
			store->Release();
		}


		if (!found)
			device->Release();
	}
	LOG_ERROR(GENERAL, "WASAPI: Custom device \"%s\" was not found", preferredDevice);
	devices->Release();
}

void WASAPIBackend::Open(u32)
{
	IMMDevice *device = nullptr;

	if (has_custom_device_set())
		findPreferredDevice(&device);

	if (device == nullptr) {
		LOG_NOTICE(GENERAL, "Looking for default audio device");
		if (!check("No default audio device found", m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
			goto cleanup;
		if (!check("Could not activate device", device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, NULL,
																 reinterpret_cast<LPVOID*>(&m_client))))
			goto cleanup;
	}

	REFERENCE_TIME period = 0;
	if (!check("Could not get device period", m_client->GetDevicePeriod(nullptr, &period)))
		goto cleanup;

	AUDCLNT_SHAREMODE sharemode = g_cfg.audio.exclusive_mode ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
	if (!check("Could not initialize client", m_client->Initialize(sharemode,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST, period, period,
			reinterpret_cast<WAVEFORMATEX*>(&m_format), NULL)))
		goto cleanup;

	if (!check("Could not open renderer", m_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<LPVOID*>(&m_renderer))))
		goto cleanup;

	check("Could not start stream", m_client->Start());

	LOG_SUCCESS(GENERAL, "WASAPI initialized");

	cleanup:
	if (device)
	{
		device->Release();
		device = nullptr;
	}
}

void WASAPIBackend::Close()
{
	m_client->Stop();
	m_client->Release();
}

bool WASAPIBackend::AddData(const void *src, u32 num_samples)
{
	BYTE *data = nullptr;
	if (check("Creating buffer failed", m_renderer->GetBuffer(num_samples, &data))) {
		data = (BYTE *) src;
		return check("Buffer release failed", m_renderer->ReleaseBuffer(num_samples, 0));
	}
	return false;
}

std::vector<std::string> WASAPIBackend::GetAvailableDevices() {
	IMMDeviceCollection *devices = nullptr;
	if (!check("Could not retrieve devices", m_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
		return {};

	UINT count = 0;
	check("Error counting devices", devices->GetCount(&count));

	std::vector<std::string> vect{};
	vect.reserve(count);

	for (UINT i = 0; i < count; i++)
	{
		IMMDevice *device = nullptr;
		if (!check("Error retrieving device", devices->Item(i, &device)))
			continue;

		IPropertyStore *store = nullptr;

		if (check("Could not open device properties", device->OpenPropertyStore(STGM_READ, &store)))
		{
			PROPVARIANT property;
			PropVariantInit(&property);

			if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &property)))
			{
				LPWSTR name = property.pwszVal;

				vect.push_back(wcharToStdString(name));
			}

			store->Release();
			PropVariantClear(&property);
		}

		device->Release();
	}

	devices->Release();
	return vect;
}

void WASAPIBackend::Play() {
	check("Play failed", m_client->Start());
}

void WASAPIBackend::Pause() {
	check("Stop failed", m_client->Stop());
}

void WASAPIBackend::Flush() {
	check("Flush failed", m_client->Reset());
}

#endif
