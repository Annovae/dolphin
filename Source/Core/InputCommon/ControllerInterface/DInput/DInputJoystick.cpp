// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "DInputJoystick.h"
#include "DInput.h"

#include <map>
#include <sstream>
#include <algorithm>

#include <wbemidl.h>
#include <oleauto.h>

namespace ciface
{
namespace DInput
{

#define DATA_BUFFER_SIZE 32

//-----------------------------------------------------------------------------
// Modified some MSDN code to get all the XInput device GUID.Data1 values in a vector,
// faster than checking all the devices for each DirectInput device, like MSDN says to do
//-----------------------------------------------------------------------------
void GetXInputGUIDS( std::vector<DWORD>& guids )
{

#define SAFE_RELEASE(p) { if(p) { (p)->Release(); (p)=NULL; } }

	IWbemLocator*           pIWbemLocator  = NULL;
	IEnumWbemClassObject*   pEnumDevices   = NULL;
	IWbemClassObject*       pDevices[20]   = {0};
	IWbemServices*          pIWbemServices = NULL;
	BSTR                    bstrNamespace  = NULL;
	BSTR                    bstrDeviceID   = NULL;
	BSTR                    bstrClassName  = NULL;
	DWORD                   uReturned      = 0;
	VARIANT                 var;
	HRESULT                 hr;

	// CoInit if needed
	hr = CoInitialize(NULL);
	bool bCleanupCOM = SUCCEEDED(hr);

	// Create WMI
	hr = CoCreateInstance(__uuidof(WbemLocator),
	                      NULL,
	                      CLSCTX_INPROC_SERVER,
	                      __uuidof(IWbemLocator),
	                      (LPVOID*) &pIWbemLocator);
	if (FAILED(hr) || pIWbemLocator == NULL)
		goto LCleanup;

	bstrNamespace = SysAllocString(L"\\\\.\\root\\cimv2");if(bstrNamespace == NULL) goto LCleanup;
	bstrClassName = SysAllocString(L"Win32_PNPEntity");   if(bstrClassName == NULL) goto LCleanup;
	bstrDeviceID  = SysAllocString(L"DeviceID");          if(bstrDeviceID == NULL)  goto LCleanup;

	// Connect to WMI
	hr = pIWbemLocator->ConnectServer(bstrNamespace, NULL, NULL, 0L, 0L, NULL, NULL, &pIWbemServices);
	if (FAILED(hr) || pIWbemServices == NULL)
		goto LCleanup;

	// Switch security level to IMPERSONATE.
	CoSetProxyBlanket(pIWbemServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
	                  RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

	hr = pIWbemServices->CreateInstanceEnum(bstrClassName, 0, NULL, &pEnumDevices);
	if (FAILED(hr) || pEnumDevices == NULL)
		goto LCleanup;

	// Loop over all devices
	while (true)
	{
		// Get 20 at a time
		hr = pEnumDevices->Next(10000, 20, pDevices, &uReturned);
		if (FAILED(hr) || uReturned == 0)
			break;

		for (UINT iDevice = 0; iDevice < uReturned; ++iDevice)
		{
			// For each device, get its device ID
			hr = pDevices[iDevice]->Get(bstrDeviceID, 0L, &var, NULL, NULL);
			if (SUCCEEDED(hr) && var.vt == VT_BSTR && var.bstrVal != NULL)
			{
				// Check if the device ID contains "IG_".  If it does, then it's an XInput device
				// This information can not be found from DirectInput
				if (wcsstr(var.bstrVal, L"IG_"))
				{
					// If it does, then get the VID/PID from var.bstrVal
					DWORD dwPid = 0, dwVid = 0;
					WCHAR* strVid = wcsstr(var.bstrVal, L"VID_");
					if (strVid && swscanf(strVid, L"VID_%4X", &dwVid) != 1)
						dwVid = 0;
					WCHAR* strPid = wcsstr(var.bstrVal, L"PID_");
					if (strPid && swscanf(strPid, L"PID_%4X", &dwPid) != 1)
						dwPid = 0;

					// Compare the VID/PID to the DInput device
					DWORD dwVidPid = MAKELONG(dwVid, dwPid);
					guids.push_back(dwVidPid);
					//bIsXinputDevice = true;
				}
			}
			SAFE_RELEASE(pDevices[iDevice]);
		}
	}

LCleanup:
	if(bstrNamespace)
		SysFreeString(bstrNamespace);
	if(bstrDeviceID)
		SysFreeString(bstrDeviceID);
	if(bstrClassName)
		SysFreeString(bstrClassName);
	for (UINT iDevice = 0; iDevice < 20; iDevice++)
		SAFE_RELEASE(pDevices[iDevice]);
	SAFE_RELEASE(pEnumDevices);
	SAFE_RELEASE(pIWbemLocator);
	SAFE_RELEASE(pIWbemServices);

	if (bCleanupCOM)
		CoUninitialize();
}

void InitJoystick(IDirectInput8* const idi8, std::vector<Core::Device*>& devices, HWND hwnd)
{
	std::list<DIDEVICEINSTANCE> joysticks;
	idi8->EnumDevices( DI8DEVCLASS_GAMECTRL, DIEnumDevicesCallback, (LPVOID)&joysticks, DIEDFL_ATTACHEDONLY );

	// this is used to number the joysticks
	// multiple joysticks with the same name shall get unique ids starting at 0
	std::map< std::basic_string<TCHAR>, int> name_counts;

	std::vector<DWORD> xinput_guids;
	GetXInputGUIDS( xinput_guids );

	for (DIDEVICEINSTANCE& joystick : joysticks)
	{
		// skip XInput Devices
		if (std::find(xinput_guids.begin(), xinput_guids.end(), joystick.guidProduct.Data1) != xinput_guids.end())
			continue;

		LPDIRECTINPUTDEVICE8 js_device;
		if (SUCCEEDED(idi8->CreateDevice(joystick.guidInstance, &js_device, NULL)))
		{
			if (SUCCEEDED(js_device->SetDataFormat(&c_dfDIJoystick)))
			{
				if (FAILED(js_device->SetCooperativeLevel(GetAncestor(hwnd, GA_ROOT), DISCL_BACKGROUND | DISCL_EXCLUSIVE)))
				{
					//PanicAlert("SetCooperativeLevel(DISCL_EXCLUSIVE) failed!");
					// fall back to non-exclusive mode, with no rumble
					if (FAILED(js_device->SetCooperativeLevel(NULL, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE)))
					{
						//PanicAlert("SetCooperativeLevel failed!");
						js_device->Release();
						continue;
					}
				}

				Joystick* js = new Joystick(/*&*i, */js_device, name_counts[joystick.tszInstanceName]++);
				// only add if it has some inputs/outputs
				if (js->Inputs().size() || js->Outputs().size())
					devices.push_back(js);
				else
					delete js;
			}
			else
			{
				//PanicAlert("SetDataFormat failed!");
				js_device->Release();
			}
		}
	}
}

Joystick::Joystick( /*const LPCDIDEVICEINSTANCE lpddi, */const LPDIRECTINPUTDEVICE8 device, const unsigned int index )
	: m_device(device)
	, m_index(index)
	//, m_name(TStringToString(lpddi->tszInstanceName))
{
	// seems this needs to be done before GetCapabilities
	// polled or buffered data
	DIPROPDWORD dipdw;
	dipdw.diph.dwSize = sizeof(DIPROPDWORD);
	dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	dipdw.diph.dwObj = 0;
	dipdw.diph.dwHow = DIPH_DEVICE;
	dipdw.dwData = DATA_BUFFER_SIZE;
	// set the buffer size,
	// if we can't set the property, we can't use buffered data
	m_buffered = SUCCEEDED(m_device->SetProperty(DIPROP_BUFFERSIZE, &dipdw.diph));

	// seems this needs to be done after SetProperty of buffer size
	m_device->Acquire();

	// get joystick caps
	DIDEVCAPS js_caps;
	js_caps.dwSize = sizeof(js_caps);
	if (FAILED(m_device->GetCapabilities(&js_caps)))
		return;

	// max of 32 buttons and 4 hats / the limit of the data format I am using
	js_caps.dwButtons = std::min((DWORD)32, js_caps.dwButtons);
	js_caps.dwPOVs = std::min((DWORD)4, js_caps.dwPOVs);

	//m_must_poll = (js_caps.dwFlags & DIDC_POLLEDDATAFORMAT) != 0;

	// buttons
	for (u8 i = 0; i != js_caps.dwButtons; ++i)
		AddInput(new Button(i, m_state_in.rgbButtons[i]));

	// hats
	for (u8 i = 0; i != js_caps.dwPOVs; ++i)
	{
		// each hat gets 4 input instances associated with it, (up down left right)
		for (u8 d = 0; d != 4; ++d)
			AddInput(new Hat(i, m_state_in.rgdwPOV[i], d));
	}

	// get up to 6 axes and 2 sliders
	DIPROPRANGE range;
	range.diph.dwSize = sizeof(range);
	range.diph.dwHeaderSize = sizeof(range.diph);
	range.diph.dwHow = DIPH_BYOFFSET;
	// screw EnumObjects, just go through all the axis offsets and try to GetProperty
	// this should be more foolproof, less code, and probably faster
	for (unsigned int offset = 0; offset < DIJOFS_BUTTON(0) / sizeof(LONG); ++offset)
	{
		range.diph.dwObj = offset * sizeof(LONG);
		// try to set some nice power of 2 values (128) to match the GameCube controls
		range.lMin = -(1 << 7);
		range.lMax = (1 << 7);
		m_device->SetProperty(DIPROP_RANGE, &range.diph);
		// but I guess not all devices support setting range
		// so I getproperty right afterward incase it didn't set.
		// This also checks that the axis is present
		if (SUCCEEDED(m_device->GetProperty(DIPROP_RANGE, &range.diph)))
		{
			const LONG base = (range.lMin + range.lMax) / 2;
			const LONG& ax = (&m_state_in.lX)[offset];

			// each axis gets a negative and a positive input instance associated with it
			AddAnalogInputs(new Axis(offset, ax, base, range.lMin-base),
				new Axis(offset, ax, base, range.lMax-base));
		}
	}

	// force feedback
	std::list<DIDEVICEOBJECTINSTANCE> objects;
	if (SUCCEEDED(m_device->EnumObjects(DIEnumDeviceObjectsCallback, (LPVOID)&objects, DIDFT_AXIS)))
	{
		InitForceFeedback(m_device, objects.size());
	}

	ClearInputState();
}

Joystick::~Joystick()
{
	m_device->Unacquire();
	m_device->Release();
}

void Joystick::ClearInputState()
{
	ZeroMemory(&m_state_in, sizeof(m_state_in));
	// set hats to center
	memset(m_state_in.rgdwPOV, 0xFF, sizeof(m_state_in.rgdwPOV));
}

std::string Joystick::GetName() const
{
	return GetDeviceName(m_device);
}

int Joystick::GetId() const
{
	return m_index;
}

std::string Joystick::GetSource() const
{
	return DINPUT_SOURCE_NAME;
}

// update IO

bool Joystick::UpdateInput()
{
	HRESULT hr = 0;

	// just always poll,
	// MSDN says if this isn't needed it doesn't do anything
	m_device->Poll();

	if (m_buffered)
	{
		DIDEVICEOBJECTDATA evtbuf[DATA_BUFFER_SIZE];
		DWORD numevents = DATA_BUFFER_SIZE;
		hr = m_device->GetDeviceData(sizeof(*evtbuf), evtbuf, &numevents, 0);

		if (SUCCEEDED(hr))
		{
			for (LPDIDEVICEOBJECTDATA evt = evtbuf; evt != (evtbuf + numevents); ++evt)
			{
				// all the buttons are at the end of the data format
				// they are bytes rather than longs
				if (evt->dwOfs < DIJOFS_BUTTON(0))
					*(DWORD*)(((BYTE*)&m_state_in) + evt->dwOfs) = evt->dwData;
				else
					((BYTE*)&m_state_in)[evt->dwOfs] = (BYTE)evt->dwData;
			}

			// seems like this needs to be done maybe...
			if (DI_BUFFEROVERFLOW == hr)
				hr = m_device->GetDeviceState(sizeof(m_state_in), &m_state_in);
		}
	}
	else
	{
		hr = m_device->GetDeviceState(sizeof(m_state_in), &m_state_in);
	}

	// try reacquire if input lost
	if (DIERR_INPUTLOST == hr || DIERR_NOTACQUIRED == hr)
		hr = m_device->Acquire();

	return SUCCEEDED(hr);
}

// get name

std::string Joystick::Button::GetName() const
{
	std::ostringstream ss;
	ss << "Button " << (int)m_index;
	return ss.str();
}

std::string Joystick::Axis::GetName() const
{
	std::ostringstream ss;
	// axis
	if (m_index < 6)
	{
		ss << "Axis " << (char)('X' + (m_index % 3));
		if (m_index > 2)
			ss << 'r';
	}
	// slider
	else
	{
		ss << "Slider " << (int)(m_index - 6);
	}

	ss << (m_range < 0 ? '-' : '+');
	return ss.str();
}

std::string Joystick::Hat::GetName() const
{
	static char tmpstr[] = "Hat . .";
	tmpstr[4] = (char)('0' + m_index);
	tmpstr[6] = "NESW"[m_direction];
	return tmpstr;
}

// get / set state

ControlState Joystick::Axis::GetState() const
{
	return std::max(0.0f, ControlState(m_axis - m_base) / m_range);
}

ControlState Joystick::Button::GetState() const
{
	return ControlState(m_button > 0);
}

ControlState Joystick::Hat::GetState() const
{
	// can this func be simplified ?
	// hat centered code from MSDN
	if (0xFFFF == LOWORD(m_hat))
		return 0;

	return (abs((int)(m_hat / 4500 - m_direction * 2 + 8) % 8 - 4) > 2);
}

}
}
