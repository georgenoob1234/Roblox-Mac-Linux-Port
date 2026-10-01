#ifndef RBX_KEYBOARD_MAP_H
#define RBX_KEYBOARD_MAP_H

static unsigned short macKey(unsigned scan) {
    // SDL physical scancodes are USB HID usages; Cocoa uses virtual keycodes.
    static const unsigned char letters[]={0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6};
    static const unsigned char digits[]={18,19,20,21,23,22,26,28,25,29};
    if(scan>=4 && scan<=29)return letters[scan-4];
    if(scan>=30 && scan<=39)return digits[scan-30];
    static const unsigned char functionKeys[]={122,120,99,118,96,97,98,100,101,109,103,111};
    if(scan>=58 && scan<=69)return functionKeys[scan-58];
    static const unsigned char keypad[]={71,75,67,78,69,76,83,84,85,86,87,88,89,91,92,82,65};
    if(scan>=83 && scan<=99)return keypad[scan-83];
    if(scan==103)return 81;
    // The ISO 102nd key (USB HID 0x64) carries < and > on ISO layouts such as
    // AZERTY. It was absent here, and the caller drops anything that returns
    // 0xffff, so the key produced no event at all.
    if(scan==100)return 10; // kVK_ISO_Section
    static const unsigned char highFunctionKeys[]={105,107,113,106,64,79,80,90}; // F13..F20
    if(scan>=104 && scan<=111)return highFunctionKeys[scan-104];
    switch(scan){case 40:return 36;case 41:return 53;case 42:return 51;case 43:return 48;case 44:return 49;
    case 45:return 27;case 46:return 24;case 47:return 33;case 48:return 30;case 49:return 42;case 51:return 41;case 52:return 39;case 53:return 50;case 54:return 43;case 55:return 47;case 56:return 44;
    case 57:return 57;case 73:return 114;case 74:return 115;case 75:return 116;case 76:return 117;case 77:return 119;case 78:return 121;case 79:return 124;case 80:return 123;case 81:return 125;case 82:return 126;
    case 224:return 59;case 225:return 56;case 226:return 58;case 227:return 55;case 228:return 62;case 229:return 60;case 230:return 61;case 231:return 54;default:return 0xffff;}
}

#endif
