#pragma once

#ifdef _WIN32

#include "Emu/Audio/AudioBackend.h"

#include "Audioclient.h"
#include "Windows.h"
#include "mmreg.h"
#include "mmdeviceapi.h"

class WASAPIBackend : public AudioBackend
{
private:
	WAVEFORMATEXTENSIBLE m_format;
	struct IAudioClient *m_client = nullptr;
	IMMDeviceEnumerator *m_enumerator = nullptr;
	IAudioRenderClient *m_renderer = nullptr;

	void findPreferredDevice(IMMDevice **dev);

public:
	WASAPIBackend();
	virtual ~WASAPIBackend() override;

	virtual const char* GetName() const override { return "WASAPI"; }

	static const u32 capabilities = DEVICE_SELECTION | EXCLUSIVE_MODE | PLAY_PAUSE_FLUSH;
	virtual u32 GetCapabilities() const override { return capabilities; }

	virtual void Open(u32) override;
	virtual void Close() override;

	virtual bool AddData(const void* src, u32 num_samples) override;

	std::vector<std::string> GetAvailableDevices() override;
	void Play() override;
	void Pause() override;
	void Flush() override;
};

#endif
