#ifndef __FITOM_DEFINE_H__
#define __FITOM_DEFINE_H__
#pragma once

#include <cstdint>

//Resource code
#define	DEVICE_NONE	0
#define	DEVICE_SSG	1	//YM2149
#define	DEVICE_OPN	2	//YM2203
#define	DEVICE_OPN2	3	//YM2612
#define	DEVICE_OPNA	4	//YM2608
#define	DEVICE_OPM	5	//YM2151
#define	DEVICE_OPLL	6	//YM2413
#define	DEVICE_OPL	7	//YM3526
#define	DEVICE_OPL2	8	//YM3812
#define	DEVICE_OPL3	9	//YMF262
#define DEVICE_OPN3L 10	//YMF288
#define DEVICE_OPNB 11	//YM2610
#define DEVICE_SAA	12	//SAA1099
#define DEVICE_DSG	13	//YM2163
#define	DEVICE_PSG	15	//AY-3-891x
#define	DEVICE_DCSG	16	//SN76489
#define	DEVICE_SCC	17	//SCC with ROM
#define DEVICE_SCCP	18	//SCC for SNATCHER
#define	DEVICE_SSGS	19	//YMZ705
#define	DEVICE_EPSG	20	//AY8930
#define DEVICE_SSGL 21	//YMZ284
#define DEVICE_SSGLP 22	//YMZ294
#define DEVICE_SID	23	//MOS6581/8580
#define	DEVICE_Y8950	24	//YM3801 a.k.a. MSX-AUDIO
#define	DEVICE_OPL3_2	25	//2op ch of OPL3
#define	DEVICE_OPP	26	//YM2164
#define	DEVICE_OPZ	27	//YM2414
#define	DEVICE_OPZ2	28	//YM2424
#define	DEVICE_OPLLP	29	//YMF281
#define	DEVICE_OPLL2	30	//YM2420
#define DEVICE_OPNC 31	//YMF264
#define DEVICE_OPN2C 32	//YM3438
#define DEVICE_OPN2L 33	//YMF276
#define DEVICE_2610B 34	//YM2610B
#define DEVICE_F286 35	//YMF286-K
#define DEVICE_OPN3 36	//YMF297
#define DEVICE_OPN3_L3 37	//OPL mode of YMF297
#define DEVICE_OPN3_N3 38	//OPN mode of YMF297
#define DEVICE_OPLLX	39	//YM2423/MS1823
#define DEVICE_OPK 40	//YM7116
#define DEVICE_OPK2 41	//YM7129
#define DEVICE_OPQ	42	//YM3806/YM3533
#define DEVICE_RYP4	43	//YM2154
#define DEVICE_RYP6	44	//YM3301/YM3302
#define DEVICE_FMS	45	//YMZ735/YMZ736
#define DEVICE_VRC7	46	//FS1001 (OPLL からリズムチャンネルを削除した派生)
#define DEVICE_5232	47	//MSM5232
#define DEVICE_OPL4	48	//YMF278 with YRW801
#define	DEVICE_OPL4AWM	61	//YMF278 AWM(PCM)部専用。FM部はCOPL3(DEVICE_OPL3)が別途担当
#define DEVICE_OPL4ML	49	//YMF704
#define DEVICE_OPL4ML2	50	//YMF721
#define DEVICE_MA1		51	//YMU757
#define DEVICE_MA2		52	//YMU759
#define DEVICE_MA3		53	//YMU762
#define DEVICE_MA5		54	//YMU765
#define DEVICE_MA7		55	//YMU768
#define DEVICE_SD1		56	//YMF825
#define DEVICE_PCMD8	57	//YMZ280
#define DEVICE_SSGD		58	//SSG on Digital (YMF288/YMF294)

#define	DEVICE_ADPCM	119	//virtual device for ADPCM channel
#define	DEVICE_ADPCMA	118	//YM2610/YM2610B
#define	DEVICE_ADPCMB	117	//YM2610/YM2610B (OPNB系)。OPNA用は DEVICE_ADPCMB_OPNA を使う
#define	DEVICE_ADPCMB_OPNA	60	//YM2608 (OPNA) 内蔵ADPCM-B。レジスタマップがOPNBと異なるため分離

#define DEVICE_RHYTHM	120	// Virtual device for rhythm channel

#define GDEVID_YM2203	0x2203	//OPN
#define GDEVID_YM2608	0x2608	//OPNA
#define GDEBID_YM2610	0x2610	//OPNB
#define GDEBID_YM2610B	0x261b	//OPNB-B
#define GDEVID_YM2612	0x2612	//OPN2
#define GDEVID_YM3438	0x3438	//OPN2C
#define GDEVID_YMF264	0xf264	//OPNC
#define GDEVID_YMF274	0xf274	//OPN2L
#define GDEVID_YMF286	0xf286	//OPNB-K
#define GDEVID_YMF288	0xf288	//OPN3L
#define GDEVID_YMF297	0xf297	//OPN3

#define GDEVID_YM2151	0x2151	//OPM
#define GDEVID_YM2164	0x2164	//OPP
#define GDEVID_YM2414	0x2414	//OPZ
#define GDEVID_YM2424	0x2424	//OPZ2
#define GDEVID_YMF709	0xf709	//OPOS

#define GDEVID_YM3526	0x3526	//OPL
#define GDEVID_YM3801	0x3801	//Y8950
#define GDEVID_YM3812	0x3812	//OPL2
#define GDEVID_YMF262	0xf262	//OPL3
#define GDEVID_YMF289	0xf289	//OPL3L

#define GDEVID_YMF278	0xf278	//OPL4
#define GDEVID_YMF704	0xf704	//OPL4-ML
#define GDEVID_YMF721	0xf721	//OPL4-ML2

#define GDEVID_YM3533	0x3533	//OPQ
#define GDEVID_YM3806	0x3806	//OPQ
#define GDEVID_YM7116	0x7116	//OPK
#define GDEVID_YM7129	0x7129	//OPK2

#define GDEVID_YM2413	0x2413	//OPLL
#define GDEVID_YM2420	0x2420	//OPLL2
#define GDEVID_YM2423	0x2423	//OPLL-X
#define GDEVID_YMF281	0xf281	//OPLLP

#define GDEVID_YMF271	0xf271	//OPX
#define GDEVID_YMZ280	0xf280	//PCMD8
#define GDEVID_YMF292	0xf292	//SCSP

#define GDEVID_YMZ705	0xf705	//SSGS
#define GDEVID_YMZ732	0xf732	//SSGS2
#define GDEVID_YMZ733	0xf733	//PCMS
#define GDEVID_YMZ735	0xf735	//FMS
#define GDEVID_YMF771	0xf771	//SSGS3

#define GDEVID_YMU757	0xf757	//MA-1
#define GDEVID_YMU761	0xf761	//PA-1
#define GDEVID_YMU759	0xf759	//MA-2
#define GDEVID_YMU762	0xf762	//MA-3
#define GDEVID_YMU765	0xf765	//MA-5
#define GDEVID_YMF781	0xf781	//APL-1
#define GDEVID_YMF795	0xf795	//APL-2
#define GDEVID_YMF807	0xf807	//APL-3
#define GDEVID_YMF825	0xf825	//SD-1
#define GDEVID_YMF827	0xf827	//APL-5

#define GDEVID_AY8910	0x8910	//PSG
#define GDEVID_YM2149	0x2149	//SSG
#define GDEVID_YM2163	0x2163	//DSG
#define GDEVID_AY8930	0x8930	//APSG
#define GDEVID_SAA1099	0x1099	//SAA
#define GDEVID_SN76489	0x76489	//DCSG
#define GDEVID_SN76496	0x76496	//DCSG
#define GDEVID_YM2602	0x2602	//SEGA DCSG

#define GDEVID_MOS6581	0x6581	//SID
#define GDEVID_MOS8580	0x8580	//SID
#define GDEVID_MSM5232	0x5232	//MSM

#define GDEVID_YM2212	0x2212	//SCC
#define GDEVID_YM2312	0x2312	//SCC+

#define BUILTIN_RHYTHM	64
#define DEVICE_OPNA_RHY	(DEVICE_OPNA | BUILTIN_RHYTHM)			//68
#define DEVICE_OPN3L_RHY	(DEVICE_OPN3L | BUILTIN_RHYTHM)		//74
#define DEVICE_OPL_RHY	(DEVICE_OPL | BUILTIN_RHYTHM)			//71
#define DEVICE_OPLL_RHY	(DEVICE_OPLL | BUILTIN_RHYTHM)			//70
#define DEVICE_OPK_RHY	(DEVICE_OPK | BUILTIN_RHYTHM)			//104

#define DEVICE_MULTI	1024	//Multiple module
#define DEVICE_NBV		1025	//NBV3/NBV4
#define	DEVICE_AYB		1026	//AYB01/AYB02

#define VOICE_TYPE_OPM  0x10
#define VOICE_TYPE_OPN  0x11
#define VOICE_TYPE_OPNA	 0x12
#define VOICE_TYPE_OPZ  0x13

#define VOICE_TYPE_OPL2 0x20
#define VOICE_TYPE_OPL  0x21
#define VOICE_TYPE_OPLL 0x22
#define VOICE_TYPE_OPK	0x23

#define VOICE_TYPE_OPL3 0x30
#define VOICE_TYPE_MA3	0x31

#define VOICE_TYPE_SSG  0x40
#define VOICE_TYPE_EPSG 0x41
#define VOICE_TYPE_DCSG 0x42
#define VOICE_TYPE_DSG	0x43
#define VOICE_TYPE_SAA	0x44
#define VOICE_TYPE_SCC	0x48

#define VOICE_TYPE_DRUM 0x50
#define VOICE_TYPE_PCM	0x70
#define VOICE_TYPE_OPL4 0x71

// 以下3つは CFITOM.cpp の kDevMap (死んだコード、呼び出し元なし。
// VoicePatchType システムと機能重複) が参照するために暫定追加。
// kDevMap 自体の削除・整理を検討中のため、正式な体系には組み込んでいない。
#define VOICE_TYPE_FM4  0x60
#define VOICE_TYPE_FM2  0x61
#define VOICE_TYPE_PSG  0x62

#define VOICE_GROUP_NONE 0x00
#define VOICE_GROUP_OPM  0x01
#define VOICE_GROUP_OPNA 0x02
#define VOICE_GROUP_OPL2 0x04
#define VOICE_GROUP_OPL3 0x08
#define VOICE_GROUP_OPLL 0x10
#define VOICE_GROUP_PSG  0x20
#define VOICE_GROUP_OPL4 0x40
#define VOICE_GROUP_PCM	 0x80
#define VOICE_GROUP_MA3  0x100
#define VOICE_GROUP_RHYTHM 0x8000
#define VOICE_GROUP_ALL  0xffff

#define DEVICE_CAP_NONE 0x00
#define DEVICE_CAP_FM   0x01
#define DEVICE_CAP_PSG  0x02
#define DEVICE_CAP_RHY  0x04
#define DEVICE_CAP_PCM  0x08

#define RHYTHM_BANK	DEVICE_RHYTHM
#define ADPCM_BANK	0x77

// ================================================================
//  VoicePatchType — 音色パッチ互換性分類 (0x10-0x74)
//
//  DeviceFactory の DEVICE_* (チップドライバ生成用) とは独立した分類。
//  「ボイスパラメータ・ハードウェア機能が一致するもの」だけをまとめる。
//  0 = 無効/未指定 (予約値、設定禁止)。
//
//  各ブロックの先頭値がそのまま VoiceGroup (HwBankRegistry 検索キー、
//  データフォーマット/パラメータ範囲の分類) に対応する。
// ================================================================
#define VOICE_PATCH_NONE     0x00

// 0x10: VoiceGroup=OPNA
#define VOICE_PATCH_OPN      0x10  // YM2203, YMF264
#define VOICE_PATCH_OPN2     0x11  // YM2612,YM3438,YMF276,YM2608,YMF288,YM2610,YM2610B,YMF286K

// 0x18: VoiceGroup=OPM
#define VOICE_PATCH_OPM      0x19  // YM2151, YM2164
#define VOICE_PATCH_OPZ      0x1a  // YM2414
#define VOICE_PATCH_OPZ2     0x1b  // YM2424

// 0x20: VoiceGroup=OPL2
#define VOICE_PATCH_OPL      0x20  // YM3526, YM3801
#define VOICE_PATCH_OPL2     0x21  // YM3812
// OPL3(YMF262)の2opモード。WSが3bit(8波形)まで使えるためOPL2(2bit,4波形)
// とは別分類とする。OPL2側へのフォールバックはWS<4の場合のみ許可する。
#define VOICE_PATCH_OPL3_2   0x22  // YMF264/289/278-2OP
// OPL系内蔵リズムチャンネル(COPLRhythm)専用。COPNARhythm/COPLLRhythmとは
// 異なり、リズム音がROM固定ではなく実際のFMオペレータパラメータを要求
// するため(HH/SD/TOM/CYMは1オペレータ、BDは2オペレータの混在)、
// VOICE_PATCH_NONEではなくこの専用識別子を持たせ、「音色がデバイスを
// 選択する」原則(findDeviceIndexByVoicePatchTypeがそのまま使える)を
// 保つ。HwBankの名前空間もOPL2等とは独立させる
// (voicePatchTypeToVoiceGroup→VOICE_GROUP_RHYTHM、2026年7月)。
#define VOICE_PATCH_OPL_RHY  0x23

// 0x28: VoiceGroup=OPLL
#define VOICE_PATCH_OPLL     0x28  // YM2413, YM2420
#define VOICE_PATCH_OPLLP    0x29  // YMF281
#define VOICE_PATCH_OPLLX    0x2a  // YM2423
#define VOICE_PATCH_VRC7     0x2b  // FS1001

// 0x30: VoiceGroup=OPL3
#define VOICE_PATCH_OPL3     0x30  // YMF264/289/278-4OP

// 0x38: VoiceGroup=MA3 (未実装、値のみ予約)
#define VOICE_PATCH_SD1      0x38  // YMF825
#define VOICE_PATCH_MA3      0x39
#define VOICE_PATCH_MA5      0x3a
#define VOICE_PATCH_MA7      0x3b

// 0x40: VoiceGroup=PSG (無波形メモリ)
#define VOICE_PATCH_SSG      0x40  // YM2149, AY-3-8910
#define VOICE_PATCH_EPSG     0x41  // AY8930 (DEVICE_EPSGの命名規則に統一)
#define VOICE_PATCH_DCSG     0x42  // SN76489
#define VOICE_PATCH_SAA      0x43  // SAA1099

// 0x48: VoiceGroup=PSG (波形ROM)
#define VOICE_PATCH_SCC      0x48  // SCC, SCCP

// 0x50: VoiceGroup=PCM
// (旧0x70-0x74から変更。BankSel.MSB(CC#0)による直接モードのチップ選択
//  IDとして使うため、0x01-0x6Fの範囲に収める必要がある。0x70-0x7Fは
//  将来の予約領域および GM2 リズム/メロディ切替 (0x78/0x79) 専用とする)
#define VOICE_PATCH_ADPCMB_Y8950 0x50  // Y8950
#define VOICE_PATCH_ADPCMB       0x51  // YM2608
#define VOICE_PATCH_ADPCMA       0x52  // YM2610
#define VOICE_PATCH_PCMD8        0x53  // YMZ280
#define VOICE_PATCH_AWM          0x54  // YMF278-AWM+YRW801

// 0x70: 内蔵リズム音源(builtin-rhythm)専用バンク選択子。
// 通常のVOICE_PATCH_*(特定チップを指す値)とは異なり、これ自体は
// 「内蔵リズム専用バンクへのアクセス」を意味するモード選択子。
// CC#0(またはToneLayer/DrumNoteのvoicePatchType)にこの値を指定した
// 場合、CC#32(hwBank)相当のフィールドで対象チップ(OPNA/OPLL等、
// 実際にはVOICE_PATCH_OPN2/VOICE_PATCH_OPLL等の既存定数を再利用して
// 指定する)を選び、ProgChg(hwProg)相当のフィールドで、そのチップの
// 内蔵リズムユニット内の楽器番号(0起算、チップごとに固定チャンネル数)
// を選ぶ。詳細はPatchManager::resolveBuiltinRhythm()参照。
#define VOICE_PATCH_BUILTIN_RHYTHM 0x70

// サンプルベース音源系 (ADPCM-B/ADPCM-A/PCMD8/AWM) かどうかを判定する。
// これらは HwPatch(FMオペレータ型)ではなく SampleZonePatch
// (キーゾーン+ベロシティレイヤー+波形/サンプル参照)を使う共通スキーマで
// 扱われる (PatchManager::resolve()の分岐、PatchData.hのSampleZone*参照)。
// 0x50-0x54 は連続した値として意図的に採番されているため、範囲チェックで
// 判定できる。
inline bool isSampleBasedVoicePatchType(uint8_t vpt) noexcept {
    return vpt >= VOICE_PATCH_ADPCMB_Y8950 && vpt <= VOICE_PATCH_AWM;
}

#define LOCATION_MONO	0
#define LOCATION_LEFT	1
#define LOCATION_RIGHT	2
#define LOCATION_STEREO	3
#define LOCATION_COMBO	4

#endif
