#include <ctime>
#include <cstdlib>
#include <fstream>

#include "common.h"
#include "MemoryMgr.h"
#include "GTA.h"
#include "Camera.h"
#include "debugmenu_public.h"
#include "INIReader.h"

HMODULE dllModule, hDummyHandle;
int gtaversion = -1;

int debugMenuLoaded = 0; // 1: not installed 2: installed
DebugMenuAPI gDebugMenuAPI;

// State variables
float targetSnow; // 0 to 1
int switchedRainWithSnow = 0; // 0: not processed  1: yes  2: not for current rain
bool attachedToWeather = false;

// INI configs
int snowFlakes = 400;
int bindedWeather = -1;
bool randomlySwitchRainWithSnow = false;
bool toggleableFromSCM = false;
int modeKey = VK_F8;

// Global variables of original snow fall code.
snowFlake* snowArray;
bool snowArrayInitialized = false;
float Snow; // CWeather::Snow
// float Stored_Snow; // CWeather::Stored_Snow

// I wanted these to be static member of AddSnow, but XP has problems with static var. allocation >:(
RwIm3DVertex snowVertexBuffer[] =
{
	{CVector(0.1, 0.0, 0.1), CVector(), 0xFFFFFFFF, 1.0f, 1.0f},
	{CVector(-0.1, 0.0, 0.1), CVector(), 0xFFFFFFFF, 0.0f, 1.0f},
	{CVector(-0.1, 0.0, -0.1), CVector(), 0xFFFFFFFF, 0.0f, 0.0f},
	{CVector(0.1, 0.0, 0.1), CVector(), 0xFFFFFFFF, 1.0f, 1.0f},
	{CVector(0.1, 0.0, -0.1), CVector(), 0xFFFFFFFF, 1.0f, 0.0f},
	{CVector(-0.1, 0.0, -0.1), CVector(), 0xFFFFFFFF, 0.0f, 0.0f},
};

RwImVertexIndex snowRenderOrder[] =
{
	0, 1, 2, 3, 4, 5
};

CBox snowBox;
CMatrix mat;

// my additions
void *snowRaster = nil;

float turnOffTime;
wchar unicodeMsg[32];
char asciiMsg[32];
char gxtCharMsg[32];

bool toggleKeyPressed = false;

// CWeather
float& InterpolationValue = *AddressByVersion<float*>(0x8F2520, 0, 0, 0x9787D8, 0, 0, 0xC8130C); 
float& Rain = *AddressByVersion<float*>(0x8E2BFC, 0, 0, 0x975340, 0, 0, 0xC81324);
uint16& NewWeatherType = *AddressByVersion<uint16*>(0x95CC70, 0, 0, 0xA10A2E, 0, 0, 0xC8131C); 
//uint16& OldWeatherType = *AddressByVersion<uint16*>(0x95CCEC, 0, 0, 0xA10AAA, 0, 0, 0xC81320);
float& Foggyness = *(float*)0xC81300; // only for SA atm (is it even on other games?)

addr rwim3dtraddr = AddressByVersion<addr>(0x5B6720, 0, 0, 0x65AE90, 0, 0, 0x7EF450); 
addr rwim3dendaddr = AddressByVersion<addr>(0x5B67F0, 0, 0, 0x65AF60, 0, 0, 0x7EF520);
addr rwim3dripaddr = AddressByVersion<addr>(0x5B6820, 0, 0, 0x65AF90, 0, 0, 0x7EF550);
WRAPPER void* RwIm3DTransform(RwIm3DVertex* pVerts, uint32 numVerts, RwMatrix* ltm, uint32 flags) { EAXJMP(rwim3dtraddr); }
WRAPPER bool RwIm3DEnd() { EAXJMP(rwim3dendaddr); }
WRAPPER bool RwIm3DRenderIndexedPrimitive(uint32 primType, RwImVertexIndex* indices, int32 numIndices) { EAXJMP(rwim3dripaddr); }

// Set in doDelayedThings for SA
addr rrssAddress = AddressByVersion<addr>(0x5A43C0, 0, 0, 0x649BA0, 0, 0);
WRAPPER bool RwRenderStateSet(RwRenderState state, void* thing) { EAXJMP(rrssAddress); }

WRAPPER void HelpMessageIII(wchar*, bool) { EAXJMP(0x5051E0); }
WRAPPER void HelpMessageVC(wchar*, bool, bool) { EAXJMP(0x55BFC0); }
WRAPPER void HelpMessageSA(char*, bool, bool, bool) { EAXJMP(0x588BE0); } 
#define SetHelpMessage(a) if (isSA()) { AsciiToGxtChar(a, gxtCharMsg); HelpMessageSA(gxtCharMsg, 1, 0, 0); } \
							else if (isVC()) { AsciiToUnicode(a, unicodeMsg); HelpMessageVC(unicodeMsg, 1, 0); } \
							else if (isIII()) { AsciiToUnicode(a, unicodeMsg); HelpMessageIII(unicodeMsg, 1); }

// CCullZones
addr cnrAddr = AddressByVersion<addr>(0x525CE0, 0, 0, 0x57E0E0, 0, 0, 0x72DDB0);
addr pnrAddr = AddressByVersion<addr>(0x525D00, 0, 0, 0x57E0C0, 0, 0, 0x72DDC0);
WRAPPER bool CamNoRain() { EAXJMP(cnrAddr); }
WRAPPER bool PlayerNoRain() { EAXJMP(pnrAddr); }

CCameraIII* TheCameraIII = (CCameraIII*)0x6FACF8;
CCameraVC* TheCameraVC = (CCameraVC*)0x7E4688;
CCameraSA* TheCameraSA = (CCameraSA*)0xB6F028; 

addr attachAddress = AddressByVersion<addr>(0x4B8DD0, 0, 0, 0x4DFA40, 0, 0, 0x59BD10);
addr updateRwAddress = AddressByVersion<addr>(0x4B8EC0, 0, 0, 0x4DF8F0, 0, 0, 0x59BBB0);
WRAPPER void CMatrix::Attach(RwMatrix* matrix, bool owner) { EAXJMP(attachAddress); }
WRAPPER void CMatrix::UpdateRW(void) { EAXJMP(updateRwAddress); }

addr wiCallAddress = AddressByVersion<addr>(0x48BFCE, 0, 0, 0x4A4C25, 0, 0, 0x5BF90F); 
void (*WeatherInit)();

uint32 &m_snTimeInMilliseconds = *AddressByVersion<uint32*>(0x885B48, 0, 0, 0x974B2C, 0, 0, 0xB7CB84);
float &ms_fTimeStep = *AddressByVersion<float*>(0x8E2CB4, 0, 0, 0x975424, 0, 0, 0xB7CB5C);

// SA TxdSlot funcs
WRAPPER void SA_PopCurrentTxd(void) { EAXJMP(0x7316B0); }
WRAPPER void SA_PushCurrentTxd(void) { EAXJMP(0x7316A0); }
WRAPPER void SA_SetCurrentTxd(int) { EAXJMP(0x7319C0); }
WRAPPER int SA_FindTxdSlot(char const*) { EAXJMP(0x731850); }
WRAPPER void* SA_RwTextureRead(char const*, char const*) { EAXJMP(0x7F3AC0); }

#define isUnderwater (isSA() ? *(float*)0xC8132C > 0.0f : false)
#define rainyWeather (isSA() ? 8 : 2)
/*
#define RwFrameGetMatrix(frame) (RwMatrix*)((addr)frame + 0x10)

const CVector
Multiply3x3(const CMatrix& mat, const CVector& vec)
{
	return CVector(
		mat.m_matrix.right.x * vec.x + mat.m_matrix.up.x * vec.y + mat.m_matrix.at.x * vec.z,
		mat.m_matrix.right.y * vec.x + mat.m_matrix.up.y * vec.y + mat.m_matrix.at.y * vec.z,
		mat.m_matrix.right.z * vec.x + mat.m_matrix.up.z * vec.y + mat.m_matrix.at.z * vec.z);
}

const CVector
Multiply3x3(const CVector& vec, const CMatrix& mat)
{
	return CVector(
		mat.m_matrix.right.x * vec.x + mat.m_matrix.right.y * vec.y + mat.m_matrix.right.z * vec.z,
		mat.m_matrix.up.x * vec.x + mat.m_matrix.up.y * vec.y + mat.m_matrix.up.z * vec.z,
		mat.m_matrix.at.x * vec.x + mat.m_matrix.at.y * vec.y + mat.m_matrix.at.z * vec.z);
}
*/

void doDelayedThings() {
/*	if (!debugMenuLoaded) {
		if (DebugMenuLoad()) {
			DebugMenuAddCmd("LCSSnow", "You're using r1.", nil);
			DebugMenuAddVarBool8("SACarCam", "Don't keep camera over water", (int8*)&seeUnderwater, nil);
			debugMenuLoaded = 2;
		} else
			debugMenuLoaded = 1;
	}
*/
	char modulePath[MAX_PATH];
	GetModuleFileName(dllModule, modulePath, MAX_PATH);
	char* p = strrchr(modulePath, '\\');
	if (p) p[1] = '\0';
	sprintf(modulePath, "%sLCSSnow.ini", modulePath);

	INIReader ini(modulePath);

	if (ini.ParseError() == 0) {
		std::string key;
		// Missing entries won't affect the variables.
		snowFlakes = ini.GetInteger("LCSSnow", "MaxSnowFlakes", snowFlakes);
		bindedWeather = ini.GetInteger("LCSSnow", "BindToWeather", bindedWeather);
		randomlySwitchRainWithSnow = ini.GetBoolean("LCSSnow", "RandomlySwitchRainWithSnow", randomlySwitchRainWithSnow);
		toggleableFromSCM = ini.GetBoolean("LCSSnow", "ToggleableFromSCM", toggleableFromSCM);
		key = ini.Get("LCSSnow", "ModeSelectKey", std::to_string(modeKey));
		modeKey = std::stoul(key, nullptr, 16);
	}

	// RwEngineInstance->dOpenDevice.fpRenderStateSet
	if(isSA())
		rrssAddress = *(addr*)((*(addr*)0xC97B24) + 32);

	snowArray = new snowFlake[snowFlakes];
	WeatherInit();
}

void
AsciiToUnicode(const char* src, wchar* dst)
{
	while ((*dst++ = *src++) != '\0');
}

void
AsciiToGxtChar(const char* src, char* dst)
{
	while ((*dst++ = *src++) != '\0');
}

// CWeather::AddSnow
template<class CameraClass>
void AddSnow(CameraClass* TheCamera)
{
	// InterpolationValue is a value between 0 and 1 corresponds to current minute 0-60

	// cycle through snow modes
	if (GetAsyncKeyState(modeKey) & (1 << 15)) {
		if (!toggleKeyPressed) {
			toggleKeyPressed = true;
			targetSnow += 1.0f / 3;

			if (targetSnow > 1.0f) {
				turnOffTime = InterpolationValue * 4.0f;
				targetSnow = 0.0f;
			}
			
			sprintf(asciiMsg, "Snow speed: %.1f", targetSnow);
			SetHelpMessage(asciiMsg);
		}
	} else
			toggleKeyPressed = false;
/*
	static bool keystate2 = false;

	// To activate rainy weather for debugging
	if (GetAsyncKeyState(VK_F7) & (1 << 15)) {
		if (!keystate2) {
			keystate2 = true;
			OldWeatherType = NewWeatherType;
			NewWeatherType = rainyWeather;
		}
	} else
		keystate2 = false;
*/
	if (randomlySwitchRainWithSnow) {
		if (NewWeatherType == rainyWeather) {
			if (switchedRainWithSnow == 0) {
				if (rand() & 1) {
					targetSnow = 1.0f;
					switchedRainWithSnow = 1;
				} else
					switchedRainWithSnow = 2;
			}
		} else {
			if(switchedRainWithSnow == 1)
				targetSnow = 0.0f;

			switchedRainWithSnow = 0;
		}
	}

	if (bindedWeather != -1) {
		if (NewWeatherType == bindedWeather && !attachedToWeather) {
			targetSnow = 1.0f;
			attachedToWeather = true;
		} else if (NewWeatherType != bindedWeather && attachedToWeather) {
			targetSnow = 0.0f;
			attachedToWeather = false;
		}
	}

	if (toggleableFromSCM) {
		// only for SA atm.
		if (isSA()) {
			if (*(int*)0xC812D4 == 1) {
				targetSnow = 1.0f;
			} else if(*(int*)0xC812D4 == 2) {
				targetSnow = 0.0f;
				*(int*)0xC812D4 = 0;
			}
		}
	}

	// CWeather::Update - Modified it a bit to add cutting snow slowly and targetSnow
	if (targetSnow != 0.0f || Snow != 0.0f) { // Weather == SNOW

		if (targetSnow == 0.0f) {
			// my addition: cut snow slowly
			Snow -= 0.25f * (InterpolationValue * 4.0f - turnOffTime);

			Snow = clamp(Snow, 0.0f, 1.0f);
		} else {
			// original code: increase snow for half a hour and decrease & stop it for other half a hour
			Snow = InterpolationValue * 4.0f;

			if (Snow > 2.0f)
				Snow -= 2 * (Snow - 2.0f);

			/*
			if (Snow > 1.0f)
				Snow = 1.0f;
			*/
			Snow = clamp(Snow, 0.0f, targetSnow);
		}
	} else {
		// Snow = 0.0f;
		return;
	}

	// CWeather::AddSnow

	if (!CamNoRain() && !PlayerNoRain() && !isUnderwater
		/*((float *)pTimeCycle + 2810) <= 0.0 &&*/ // new and unknown timecyc property that exists on many things on LCS
		/* Snow > 0.0f */) {

		int snowAmount = min(snowFlakes, Snow * snowFlakes); // s0

		//	snowBox.min = {0.0f, 0.0f, 0.0f};
		//	snowBox.max = {0.0f, 0.0f, 0.0f};
		snowBox.Set(TheCamera->GetPosition(), TheCamera->GetPosition());
		snowBox.min.x -= 40.0f;
		snowBox.min.y -= 40.0f;
		snowBox.max.x += 40.0f;
		snowBox.min.z -= 15.0f; // -= 10.0f; in PSP
		snowBox.max.z += 15.0f; // += 10.0f; in PSP
		snowBox.max.y += 40.0f;
		if (!snowRaster) {
			if (isSA()) {
				SA_PushCurrentTxd();
				SA_SetCurrentTxd(SA_FindTxdSlot("particle"));
				snowRaster = *((void**)SA_RwTextureRead("shad_exp", 0));
				SA_PopCurrentTxd();
			} else {
				snowRaster = *AddressByVersion<void**>(0x648F20, 0, 0, 0x77E6F8, 0, 0);
			}
		}

		if (!snowArrayInitialized)
		{
			snowArrayInitialized = true;
			for (int i = 0; i < snowFlakes; i++)
			{
				snowArray[i].pos.x = snowBox.min.x + ((snowBox.max.x - snowBox.min.x) * (rand() / (float)RAND_MAX));
				snowArray[i].pos.y = snowBox.min.y + ((rand() / (float)RAND_MAX) * (snowBox.max.y - snowBox.min.y));
				snowArray[i].pos.z = snowBox.min.z + ((rand() / (float)RAND_MAX) * (snowBox.max.z - snowBox.min.z));
				snowArray[i].xChange = 0.0f;
				snowArray[i].yChange = 0.0f;
			}
		}

		RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)0);
		RwRenderStateSet(rwRENDERSTATETEXTURERASTER, snowRaster); // smokeII_3
		RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)1);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)0);
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)1);
		RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)2); // 1 in psp, 5 in mobile
		RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)2); // 1 in psp, 6 in mobile

		// codes after this point doesn't exist on leftover snow code of Mobile.

		// RwError things

		mat = TheCamera->GetMatrix();

		// there was a condition here which is never meant to be met imo

		int i = 0; // s2

		for (; i < snowAmount; i++)
		{
			float& xPos = snowArray[i].pos.x; // s3
			float& yPos = snowArray[i].pos.y;
			float& zPos = snowArray[i].pos.z;
			float& xChangeRate = snowArray[i].xChange; // s4
			float& yChangeRate = snowArray[i].yChange; // s5

			float minChange = -ms_fTimeStep / 10;
			float maxChange = ms_fTimeStep / 10;

			zPos -= maxChange;

			xChangeRate += minChange + (2 * maxChange * (rand() / (float)RAND_MAX));

			yChangeRate += minChange + (2 * maxChange * (rand() / (float)RAND_MAX));

			xChangeRate = clamp(xChangeRate, minChange, maxChange);
			yChangeRate = clamp(yChangeRate, minChange, maxChange);

			yPos += yChangeRate;
			xPos += xChangeRate;
			
			while (zPos < snowBox.min.z) {
				zPos += 30.0f; // += 20.0f; in PSP
			}

			while (zPos > snowBox.max.z) {
				zPos -= 30.0f; // -= 20.0f; in PSP
			}

			while (xPos < snowBox.min.x) {
				xPos += 80.0f;
			}

			while (xPos > snowBox.max.x) {
				xPos -= 80.0f;
			}

			while (yPos < snowBox.min.y) {
				yPos += 80.0f;
			}

			while (yPos > snowBox.max.y) {
				yPos -= 80.0f;
			}

			mat.GetPosition() = snowArray[i].pos;

			if (RwIm3DTransform(snowVertexBuffer, ARRAY_SIZE(snowVertexBuffer), (RwMatrix*)&mat, 1)) {
				// PSP doesn't do it in indexed fashion, but this is only function we have in III/VC.
				RwIm3DRenderIndexedPrimitive(3 /*rwPRIMTYPETRILIST*/, snowRenderOrder, ARRAY_SIZE(snowRenderOrder));
				RwIm3DEnd();
			}
			// PSP:
			// Render3DTransform(mat, 1);
			// Render3DPrimitive(3, snowVertex, 6, 0);
		}

		RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)1);
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)1);

		// my addition
		RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)5);
		RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)6);
		RwRenderStateSet(rwRENDERSTATEFOGENABLE, 0);
		RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, 0);

	}
}

// --- VC specific hooks start

float snowThresholdShelter = 0.2f;
float snowThresholdPedModel = 0.1f;

// 4E9EB5
void __declspec(naked)
UseNearbyAttractors1_hook(void)
{
	__asm {
		mov		ebp, eax
		fld     Snow
		fcomp   snowThresholdShelter
		fnstsw  ax
		test    ah, 1
		jz      runIt

		mov		eax, ebp
		cmp     byte ptr [eax+640h], 0
		push	0x4E9EBC
		retn
runIt:
		push    0x4E9EC6
		retn
	}
}

// 4EA21A
void __declspec(naked)
UseNearbyAttractors2_hook(void)
{
	__asm {
		xor		edx, edx
		test    ah, 1
		jz      increase
		fld     Snow
		fcomp   snowThresholdShelter
		fnstsw  ax
		test    ah, 5
		jnz     dontIncrease
increase:
		inc     edx
dontIncrease:
		push    0x4EA226
		retn
	}
}

// 4EA3AF
void __declspec(naked)
UseNearbyAttractors3_hook(void)
{
	__asm {
		xor		ecx, ecx
		test    ah, 1
		jz      increase
		fld     Snow
		fcomp   snowThresholdShelter
		fnstsw  ax
		test    ah, 5
		jnz     dontIncrease
increase:
		inc     ecx
dontIncrease:
		push    0x4EA3C2
		retn
	}
}

WRAPPER int GetPedAttractorManager(void) { EAXJMP(0x62D030); }

// 51D05B
void __declspec(naked)
ProcessObjective1_hook(void)
{
	__asm {
		fld     Snow
		fcomp   snowThresholdShelter
		fnstsw  ax
		test    ah, 1
		jz		keepInShelter
		push    ebx
		push    ebp
		call    GetPedAttractorManager
		push	0x51D062
		retn

keepInShelter:
		push	0x520CEC
		retn
	}
}

// 51E895
void __declspec(naked)
ProcessObjective2_hook(void)
{
	__asm {
		fld     Snow
		fcomp   snowThresholdShelter
		fnstsw  ax
		test    ah, 1
		jz		keepInShelter
		mov		eax, [ebp+3BCh]
		push	0x51E89B
		retn

keepInShelter:
		push	0x520DD7
		retn
	}
}

// 53AFE3
void __declspec(naked)
ChooseNextCivilianOccupation_hook(void)
{
	__asm {
		test    ah, 41h
		push    esi
		push    ebp
		jz		itRains
		fld     Snow
		fcomp   snowThresholdPedModel
		fnstsw  ax
		test    ah, 41h
		jz		itRains

		push	0x53B033
		retn

itRains:
		push	0x53AFEA
		retn
	}
}

// 53B084
void __declspec(naked)
ChooseCivilianOccupation_hook(void)
{
	__asm {
		mov     ebp, [esp+1Ch+4]
		test    ah, 41h
		jz		itRains
		fld     Snow
		fcomp   snowThresholdPedModel
		fnstsw  ax
		test    ah, 41h
		jz		itRains

		push	0x53B100
		retn

itRains:
		push	0x53B08D
		retn
	}
}

// 53C0C6
void __declspec(naked)
AddToPopulation1_hook(void)
{
	__asm {
		test    ah, 41h
		jz		itRains
		fld     Snow
		fcomp   snowThresholdPedModel
		fnstsw  ax
		test    ah, 41h
		jz		itRains

		push	0x53C140
		retn

itRains:
		xor     ebp, ebp
		push	0x53C0CD
		retn
	}
}

// 53CC3D
void __declspec(naked)
AddToPopulation2_hook(void)
{
	__asm {
		test    ah, 41h
		jz		itRains
		fld     Snow
		fcomp   snowThresholdPedModel
		fnstsw  ax
		test    ah, 41h
		jz		itRains

		push	0x53CC45
		retn

itRains:
		push	0x53CC9D
		retn
	}
}

// 57D860
void __declspec(naked)
WeatherUpdate_hookVC(void)
{
	__asm {
		cmp		switchedRainWithSnow, 1
		jz		stopRain
		mov		edi, NewWeatherType
		cmp     [edi], 2
		jz		rain

		// look for hurricane
		push	0x57D86A
		retn
rain:
		push	0x57D878
		retn
stopRain:
		push	0x57D910
		retn
	}
}

// --- VC specific hooks end

// 522F0A
void __declspec(naked)
WeatherUpdate_hookIII(void)
{
	__asm {
		mov		eax, NewWeatherType
		cmp     [eax], 2
		jnz     stopRain
		cmp		switchedRainWithSnow, 1
		jz		stopRain
		push	0x522F14
		retn

stopRain:
		push	0x522F90
		retn
	}
}

// --- SA specific hooks start

float fOne = 1.0f;
float fZero = 0.0f;

// 72BBE1
void __declspec(naked)
WeatherUpdate_hookSA(void)
{
	__asm {
		cmp     cx, 10h
		fld     fZero
		jz      short itsRainyWeather
		cmp     cx, 8
		jnz     short isOldWeatherRainy
itsRainyWeather:
		cmp		switchedRainWithSnow, 1
		jz		short isOldWeatherRainy
		fstp    st
		mov		eax, InterpolationValue
		fld     [eax]
isOldWeatherRainy:
		cmp     dx, 10h
		jz      short oldWeatherWasRainy
		cmp     dx, 8
		jnz     short continueCode
oldWeatherWasRainy:
		cmp		switchedRainWithSnow, 1
		jz      short continueCode
		fld     fOne
		mov		eax, InterpolationValue
		fsub	[eax]
		faddp   st(1), st
continueCode:
		push	0x72BC15
		retn
	}
}

// something with coronas
float *flt_C812D0 = (float*)0xC812D0;

// 72C6FA
void __declspec(naked)
WeatherUpdate2_hookSA(void)
{
	__asm {
		mov		ecx, Foggyness
		fld     [ecx]
		mov		edx, Rain
		fcomp   [edx]
		fnstsw  ax
		test    ah, 41h
		jnz     short cmpRainWithSnow
		fld     [ecx]
		mov		edx, ecx // eds will be biggest value ptr now on
		jmp     short cmpFoggynessWithSnow
cmpRainWithSnow:
		fld     [edx]
		// mov		edx, edx // eds will be biggest value ptr now on
cmpFoggynessWithSnow:
		fcomp   Snow
		fnstsw  ax
		test    ah, 41h
		jnz     short biggestIsSnow
		jmp     short contWithPreviousBiggest
biggestIsSnow:
		mov		edx, offset Snow
contWithPreviousBiggest:
		fld		[edx]
		fld     fOne
		fcomp   st(1)
		fnstsw  ax
		fstp    st
		test    ah, 5
		jp      short storeBiggestValToCoronaThing
		mov		ecx, flt_C812D0
		mov     [ecx], 3F800000h
		jmp		continueCode

storeBiggestValToCoronaThing:
		mov		ecx, flt_C812D0
		mov		edx, [edx]
		mov     [ecx], edx

continueCode:
		push	 0x72C765
		retn
	}
}

// 6D2C0F
void __declspec(naked)
VehicleMakeDirty_hookSA(void)
{
	__asm {
		mov		eax, Rain
		fld     [eax]
		fcomp   snowThresholdShelter
		fnstsw  ax
		test    ah, 1
		jz      cleanCar

		fld     Snow
		fcomp   snowThresholdShelter
		fnstsw  ax
		test    ah, 1
		jz      cleanCar

		push	0x6D2C26
		retn

cleanCar:
		push	0x6D2D19
		retn
	}
}

// --- SA specific hooks end

void (*RenderRainStreaks)(void);
void
AddSnow_hookIII()
{
	AddSnow<CCameraIII>(TheCameraIII);
	RenderRainStreaks();
}

void
AddSnow_hookVC()
{
	AddSnow<CCameraVC>(TheCameraVC);
	RenderRainStreaks();
}

void
AddSnow_hookSA()
{
	AddSnow<CCameraSA>(TheCameraSA);
	RenderRainStreaks();
}

BOOL WINAPI
DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		dllModule = hInst;
/*
		// Taken from SkyGFX
		if (GetAsyncKeyState(VK_F8) & 0x8000) {
			AllocConsole();
			freopen("CONIN$", "r", stdin);
			freopen("CONOUT$", "w", stdout);
			freopen("CONOUT$", "w", stderr);
		}
*/
		GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)& DllMain, &hDummyHandle);

		srand((uint32)time(0));

		doVersionCheck();

		if (isIII()) {
			InterceptCall(&RenderRainStreaks, AddSnow_hookIII, 0x48E06D);

			// don't enable the rain when we switched it with snow
			InjectHook(0x522F0A, WeatherUpdate_hookIII, PATCH_JUMP);

		} else if (isVC()) {
			InterceptCall(&RenderRainStreaks, AddSnow_hookVC, 0x4A65C3);

			// VC / ReLCS extra: shelter and population things
			InjectHook(0x4E9EB5, UseNearbyAttractors1_hook, PATCH_JUMP);
			InjectHook(0x4EA21A, UseNearbyAttractors2_hook, PATCH_JUMP);
			InjectHook(0x4EA3AF, UseNearbyAttractors3_hook, PATCH_JUMP);
			InjectHook(0x51D05B, ProcessObjective1_hook, PATCH_JUMP);
			InjectHook(0x51E895, ProcessObjective2_hook, PATCH_JUMP);
			InjectHook(0x53B084, ChooseCivilianOccupation_hook, PATCH_JUMP);
			InjectHook(0x53AFE3, ChooseNextCivilianOccupation_hook, PATCH_JUMP);
			InjectHook(0x53C0C6, AddToPopulation1_hook, PATCH_JUMP);
			InjectHook(0x53CC3D, AddToPopulation2_hook, PATCH_JUMP);

			// don't enable the rain when we switched it with snow
			InjectHook(0x57D860, WeatherUpdate_hookVC, PATCH_JUMP);

		} else if (isSA()) {
			InterceptCall(&RenderRainStreaks, AddSnow_hookSA, 0x53E126); 

			// SA extra: clean cars while it's snowing
			InjectHook(0x6D2C0F, VehicleMakeDirty_hookSA, PATCH_JUMP);

			// don't enable the rain when we switched it with snow
			InjectHook(0x72BBE1, WeatherUpdate_hookSA, PATCH_JUMP);
			// some corona thing
			InjectHook(0x72C6FA, WeatherUpdate2_hookSA, PATCH_JUMP);
		} else
			return FALSE;

		InterceptCall(&WeatherInit, doDelayedThings, wiCallAddress);
	}
	return TRUE;
}
