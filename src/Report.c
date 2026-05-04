//
// Report.c  -  Neptune raw report -> standard HID input report translation
//
// Source of truth for the wire layout & button bitmasks:
// neptune-hidapi.net (Hid/HidEnums.cs::SDCInput, SDCButton0..5).
//
#include "Driver.h"

// HID descriptor button bit positions (must match Hid.c report descriptor)
#define HID_BTN_A       (1u << 0)
#define HID_BTN_B       (1u << 1)
#define HID_BTN_X       (1u << 2)
#define HID_BTN_Y       (1u << 3)
#define HID_BTN_LB      (1u << 4)
#define HID_BTN_RB      (1u << 5)
#define HID_BTN_VIEW    (1u << 6)   // Options / Back / Select
#define HID_BTN_MENU    (1u << 7)   // Menu / Start
#define HID_BTN_LS      (1u << 8)
#define HID_BTN_RS      (1u << 9)
#define HID_BTN_L4      (1u << 10)
#define HID_BTN_L5      (1u << 11)
#define HID_BTN_R4      (1u << 12)
#define HID_BTN_R5      (1u << 13)
#define HID_BTN_STEAM   (1u << 14)
#define HID_BTN_QAM     (1u << 15)

// 2% deadzone in 16-bit units
#define STICK_DEADZONE  655

static FORCEINLINE CHAR
_Scale16to8(_In_ SHORT v, _In_ SHORT dz)
{
    if (v > -dz && v < dz) return 0;
    SHORT s = (SHORT)(v >> 8);
    if (s >  127) s =  127;
    if (s < -127) s = -127;   // keep range symmetric so caller-side negation can't overflow
    return (CHAR)s;
}

// Steam Deck triggers report 0..32767 (15-bit unsigned in INT16). Clamp
// negatives to 0, then >>7 maps the active range to 0..255 for HID.
static FORCEINLINE UCHAR
_ScaleTrigger(_In_ SHORT v)
{
    if (v <= 0)         return 0;
    if (v >= 32640)     return 255;   // 255 << 7 = 32640
    return (UCHAR)(v >> 7);
}

// D-Pad -> HID Hat Switch (0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW 8=neutral)
static UCHAR
_DpadToHat(_In_ USHORT b0)
{
    BOOLEAN up    = (b0 & NEPTUNE_BTN0_DPAD_UP)    != 0;
    BOOLEAN down  = (b0 & NEPTUNE_BTN0_DPAD_DOWN)  != 0;
    BOOLEAN left  = (b0 & NEPTUNE_BTN0_DPAD_LEFT)  != 0;
    BOOLEAN right = (b0 & NEPTUNE_BTN0_DPAD_RIGHT) != 0;
    if (up   && right) return 1;
    if (down && right) return 3;
    if (down && left)  return 5;
    if (up   && left)  return 7;
    if (up)            return 0;
    if (right)         return 2;
    if (down)          return 4;
    if (left)          return 6;
    return 8;
}

VOID
SteamDeckReport_Translate(
    _In_  PNEPTUNE_RAW_REPORT          Raw,
    _Out_ PSTREAMDECK_HID_INPUT_REPORT Hid)
{
    USHORT  b0 = Raw->Buttons0;
    UCHAR   b1 = Raw->Buttons1;
    UCHAR   b2 = Raw->Buttons2;
    UCHAR   b4 = Raw->Buttons4;
    UCHAR   b5 = Raw->Buttons5;
    USHORT  hb = 0;

    RtlZeroMemory(Hid, sizeof(*Hid));
    Hid->ReportId = NEPTUNE_REPORT_ID_INPUT;

    // Face buttons / shoulder / triggers-as-digital / system buttons (Buttons0)
    if (b0 & NEPTUNE_BTN0_A)        hb |= HID_BTN_A;
    if (b0 & NEPTUNE_BTN0_B)        hb |= HID_BTN_B;
    if (b0 & NEPTUNE_BTN0_X)        hb |= HID_BTN_X;
    if (b0 & NEPTUNE_BTN0_Y)        hb |= HID_BTN_Y;
    if (b0 & NEPTUNE_BTN0_L1)       hb |= HID_BTN_LB;
    if (b0 & NEPTUNE_BTN0_R1)       hb |= HID_BTN_RB;
    if (b0 & NEPTUNE_BTN0_OPTIONS)  hb |= HID_BTN_VIEW;
    if (b0 & NEPTUNE_BTN0_MENU)     hb |= HID_BTN_MENU;
    if (b0 & NEPTUNE_BTN0_STEAM)    hb |= HID_BTN_STEAM;
    if (b0 & NEPTUNE_BTN0_L5)       hb |= HID_BTN_L5;

    // Stick clicks live in different button bytes
    if (b1 & NEPTUNE_BTN1_LSTICK_PRESS) hb |= HID_BTN_LS;
    if (b2 & NEPTUNE_BTN2_RSTICK_PRESS) hb |= HID_BTN_RS;

    // R5 is in Buttons1; L4/R4 are in Buttons4
    if (b1 & NEPTUNE_BTN1_R5)       hb |= HID_BTN_R5;
    if (b4 & NEPTUNE_BTN4_L4)       hb |= HID_BTN_L4;
    if (b4 & NEPTUNE_BTN4_R4)       hb |= HID_BTN_R4;

    // Quick Access Menu sits in its own byte
    if (b5 & NEPTUNE_BTN5_QUICK_ACCESS) hb |= HID_BTN_QAM;

    Hid->Buttons = hb;

    // D-Pad -> hat switch
    Hid->Hat = _DpadToHat(b0);

    // Sticks: device already reports up = positive, matching XInput / our
    // gamepad layer. No inversion needed.
    Hid->LeftStickX  = _Scale16to8(Raw->LeftStickX,  STICK_DEADZONE);
    Hid->LeftStickY  = _Scale16to8(Raw->LeftStickY,  STICK_DEADZONE);
    Hid->RightStickX = _Scale16to8(Raw->RightStickX, STICK_DEADZONE);
    Hid->RightStickY = _Scale16to8(Raw->RightStickY, STICK_DEADZONE);

    // Triggers: device gives INT16 0..32767, descriptor wants UCHAR 0..255
    Hid->LeftTrigger  = _ScaleTrigger(Raw->LeftTrigger);
    Hid->RightTrigger = _ScaleTrigger(Raw->RightTrigger);
}
