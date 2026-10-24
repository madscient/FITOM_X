#pragma once
// plugin_sdk/include/fitom/IHWPlugin.h
//
// ハードウェア I/F バックエンド DLL が実装・エクスポートする C API。
//
// ─── 設計原則 ────────────────────────────────────────────────────────────────
//   hw::HWControllerBase の write / reset / isOpen を C 関数にフラット化。
//   DLL は不透明ハンドル (HWHandle) を管理し、FITOM コアはその値を保持するだけ。
//   HW I/F プラグイン (実機の FitomHwIF、エミュレータの FitomEmuIF 等) が、
//   この C API を実装した共有ライブラリをビルドすることで FITOM コアと分離できる。
//
// ─── デバイス列挙 ────────────────────────────────────────────────────────────
//   HWPlugin_Enumerate で接続済みデバイスの JSON 文字列を返す。
//   フォーマット:
//     [
//       { "type": "RE1", "serial": "ABCD1234", "index": 0 },
//       { "type": "SPFM_TOWER", "port": "COM3", "index": 0 }
//     ]
//
// ─── アドレス変換規則 ─────────────────────────────────────────────────────────
//   HWPlugin_Write(handle, addr, data):
//     addr 上位 8bit → a_high (SPFM 拡張アドレス)
//     addr 下位 8bit → addr (& ADDR_MASK)
//   これは IPort::write() と同じ慣習。

#include <cstdint>
#include <stddef.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef FITOM_HW_PLUGIN_EXPORTS
#    define FITOM_HWP_API __declspec(dllexport)
#  else
#    define FITOM_HWP_API __declspec(dllimport)
#  endif
#  define FITOM_HWP_CALL __cdecl
#else
#  if defined(FITOM_HW_PLUGIN_EXPORTS) && defined(__GNUC__)
#    define FITOM_HWP_API __attribute__((visibility("default")))
#  else
#    define FITOM_HWP_API
#  endif
#  define FITOM_HWP_CALL
#endif

typedef enum HWResult {
    HW_OK              =  0,
    HW_ERR_NOT_FOUND   = -1,
    HW_ERR_OPEN_FAILED = -2,
    HW_ERR_IO          = -3,
    HW_ERR_INVALID_ARG = -4,
} HWResult;

struct HWDeviceOpaque;
typedef struct HWDeviceOpaque* HWHandle;

#ifdef __cplusplus
extern "C" {
#endif

// ─── プラグイン情報 ──────────────────────────────────────────────────────────
// プラグイン名を返す ("FitomHwIF", "FitomEmuIF" 等)
FITOM_HWP_API const char* FITOM_HWP_CALL HWPlugin_GetName();

// ─── 初期化 ──────────────────────────────────────────────────────────────────
// プラグインを初期化する。FITOM は DLL ロード後、他の関数を呼ぶ前に必ず呼ぶこと。
// profile_path: プラグイン固有の設定ファイルパス。nullptr または空文字でデフォルト探索。
// 戻り値: HW_OK = 成功、HW_ERR_INVALID_ARG = プロファイル解析失敗、
//         HW_ERR_OPEN_FAILED = デバイス/ストリーム起動失敗
// 初期化前に他の関数を呼んだ場合の動作は未定義（実装は失敗を返すこと）。
FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Init(const char* profile_path);

// プラグイン全体を安全に停止する(2026年7月新設、任意実装)。
// HWPlugin_Init成功後、プロセス終了前にFITOM_X側が一度だけ呼ぶ。
// オーディオストリーム等、プラグインが内部で保持するバックグラウンド
// スレッドを、この関数呼び出しの中で同期的に停止・joinすること。
//
// 背景: HWPlugin_Closeは個別ハンドル単位の解放のみを行う契約であり、
// プラグイン全体で共有するリソース(例: RtAudioストリームとその
// コールバックスレッド)を止める手段が無かった。その結果、ストリームの
// 停止がC++の静的デストラクタ実行(プロセス終了時、DLLアンロードと
// 前後する暗黙のタイミング)任せになり、Windows環境でローダーロックに
// 起因するデッドロック・フリーズを引き起こすことがある
// (FreeLibrary/dlcloseがDLL内部のスレッド停止処理と競合するため)。
//
// この関数をエクスポートしないプラグインとの後方互換のため、FITOM_X側
// はシンボルが見つからない場合は単に呼び出しをスキップする(GetProcAddress/
// dlsymの結果がnullptrなら無視、詳細はHWPluginInstance::load()参照)。
// 実装側は、この関数が呼ばれないまま(古いFITOM_X等から)プロセスが
// 終了するケースにも耐えられるよう、static/global デストラクタでの
// フォールバック停止処理自体は残しておくことを推奨する
// (ただし通常経路ではこちらが先に呼ばれ、フォールバックには到達しない)。
FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_Shutdown();

// ─── デバイス列挙 ────────────────────────────────────────────────────────────
// 接続デバイスを JSON 文字列で返す (呼び出し元は HWPlugin_FreeString で解放)
// 失敗時は nullptr
FITOM_HWP_API const char* FITOM_HWP_CALL HWPlugin_Enumerate();
FITOM_HWP_API void        FITOM_HWP_CALL HWPlugin_FreeString(const char* str);

// ─── デバイス開閉 ────────────────────────────────────────────────────────────
// params_json: { "type":"RE1", "serial":"ABCD1234", "slot":0, "clock":3579545, "pan":0 }
FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Open(
    const char* params_json, HWHandle* out_handle);
FITOM_HWP_API void     FITOM_HWP_CALL HWPlugin_Close(HWHandle handle);

// ─── I/O ─────────────────────────────────────────────────────────────────────
// addr 上位 8bit = a_high (SPFM 拡張アドレス)、下位 8bit = レジスタアドレス
FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Write(
    HWHandle handle, uint16_t addr, uint8_t data);

// バースト書き込み (startAddr は下位 8bit のみ有効)
FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_WriteBlock(
    HWHandle handle, uint8_t startAddr, const uint8_t* data, size_t len);

FITOM_HWP_API HWResult FITOM_HWP_CALL HWPlugin_Reset(HWHandle handle, unsigned int pulse_us);

// ─── メタ情報 ────────────────────────────────────────────────────────────────
FITOM_HWP_API int  FITOM_HWP_CALL HWPlugin_GetClock(HWHandle handle);
FITOM_HWP_API int  FITOM_HWP_CALL HWPlugin_GetPanpot(HWHandle handle);
FITOM_HWP_API bool FITOM_HWP_CALL HWPlugin_IsOpen(HWHandle handle);

// ─── レイテンシ同期 ──────────────────────────────────────────────────────────
// HWPlugin_GetLatencySamples:
//   このデバイスが write() から実際の発音まで要するサンプル数を返す。
//   物理チップ (SPFM 等) は 0 を返す。
//   FM エンジン内蔵 hwif は (buffer_frames) を返す。
//   FITOM コアはこの値を全デバイス間で収集し最大値を基準レイテンシとする。
FITOM_HWP_API uint32_t FITOM_HWP_CALL HWPlugin_GetLatencySamples(HWHandle handle);

// HWPlugin_SetDelaySamples:
//   FITOM コアが全デバイスの基準レイテンシを設定する。
//   物理チップはこの値だけ write() をキューイングして遅らせる。
//   FM エンジン内蔵 hwif は自身のレイテンシと一致するため何もしなくてよい。
//   delay_samples == 0 の場合は遅延なし (単デバイス構成向け)。
FITOM_HWP_API void FITOM_HWP_CALL HWPlugin_SetDelaySamples(
    HWHandle handle, uint32_t delay_samples);

// ─── 部位ごとのゲイン（任意実装）─────────────────────────────────────────────
// チップによっては、音を複数の端子から別々に出す（OPNA の FM と SSG 等）。
// この出力のひとつひとつを部位と呼び、名前の文字列で指定する
// （"FM"、"SSG" 等。大文字小文字を区別する）。実機ではボード上の回路で
// ミックスするので、音量のバランスは機種で違う。FM エンジン内蔵 hwif が、
// そのバランスを FITOM から調整できるようにするための関数。
//
// 4 関数は組でエクスポートする。FITOM は HWPlugin_GetPartCount の有無で判定し、
// 無ければどれも呼ばず、どのデバイスも部位を持たないものとして扱う
// （物理チップの hwif は実装しなくてよい）。
// HWPlugin_GetPartCount があるのに残りが欠けているプラグインは、FITOM が
// ロードに失敗させる。
//
// HWPlugin_GetPartCount:
//   デバイスが持つ部位の数を返す。部位を持たないデバイスは 0。
// HWPlugin_GetPartName:
//   index 番目の部位の名前を返す。範囲外は nullptr。
//   文字列は HWPlugin_Close(handle) まで有効（呼び出し元は解放しない）。
//   並ぶ順序は定めない。設定ファイル等に書き残すときは名前を使うこと。
// HWPlugin_SetPartGain / HWPlugin_GetPartGain:
//   gain は L/R 独立で、1.0 = 0 dB。pan による L/R の振り分けとは別に掛かる
//   （実際に掛かるのは両者の積）。
//   デバイスが持たない部位の名前と nullptr は HW_ERR_INVALID_ARG。
//   設定したゲインは HWPlugin_Close(handle) で既定値に戻る。
//   既定値は、HWPlugin_Open の直後に HWPlugin_GetPartGain で取得できる。
//   音声出力の動作中に呼び出せること。
FITOM_HWP_API uint32_t    FITOM_HWP_CALL HWPlugin_GetPartCount(HWHandle handle);
FITOM_HWP_API const char* FITOM_HWP_CALL HWPlugin_GetPartName(
    HWHandle handle, uint32_t index);
FITOM_HWP_API HWResult    FITOM_HWP_CALL HWPlugin_SetPartGain(
    HWHandle handle, const char* part, float gain_l, float gain_r);
FITOM_HWP_API HWResult    FITOM_HWP_CALL HWPlugin_GetPartGain(
    HWHandle handle, const char* part, float* out_gain_l, float* out_gain_r);

// ─── マスターボリューム（任意実装）───────────────────────────────────────────
// FITOM のマスターボリューム（GM2 Universal Realtime のマスターボリューム、
// GUI のシステム設定ダイアログ、プロファイルの master_volume）を、デバイスの
// 音声出力に掛けるための関数。音声出力を持つプラグイン（FM エンジン内蔵 hwif 等）
// が実装する。FITOM はエクスポートの有無を見て、無ければ呼ばない（物理チップの
// hwif は実装しなくてよい）。
//
//   volume: 0〜127。GM2 のマスターボリュームの上位 7bit と同じ値。
//           音量への換算はプラグインが決める。128 以上は HW_ERR_INVALID_ARG。
//   一度も呼ばれていないときは 127（減衰なし）として扱うこと。
//   部位ゲイン・pan とは別に掛かる（実際に掛かるのはこれらの積）。
//   FITOM は HWPlugin_Open したデバイスごとに同じ値を渡す。開いた後に
//   プロファイルの値で一度呼び、以後は値が変わるたびに呼ぶ。
//   chip:"SF2" のデバイスには呼ばない（SoundFont プラグインには SysEx で送る）。
//   音声出力の動作中に呼び出せること。
FITOM_HWP_API HWResult    FITOM_HWP_CALL HWPlugin_SetMasterVolume(
    HWHandle handle, uint8_t volume);

// ─── オフライン描画（任意実装）───────────────────────────────────────────────
// 音声デバイスへ出さず、FITOM が求めた分だけ音を描画して返すモード。
// SMF を WAV へ変換する fitom_render が使う。音声出力を持つプラグイン
// （FM エンジン内蔵 hwif、SoundFont プラグイン等）が実装する。
// 物理チップの hwif は実装しない（音を取り込む手段が無いため）。
//
// 2 関数は組でエクスポートする。FITOM は HWPlugin_InitOffline の有無で判定し、
// HWPlugin_InitOffline があるのに HWPlugin_Render が欠けているプラグインは、
// FITOM がロードに失敗させる。
//
// HWPlugin_InitOffline:
//   HWPlugin_Init の代わりに呼ぶ（同じプロセスで両方を呼ぶことはない）。
//   profile_path は HWPlugin_Init と同じ。音声デバイスは開かない。
//   以後は HWPlugin_Init の後と同じく、他の全関数が使えること。
//   sample_rate: 描画するサンプルレート（Hz）。プラグイン固有の設定ファイルに
//   サンプルレートの記述があっても、こちらを使う。
//   扱えないサンプルレートは HW_ERR_INVALID_ARG。
//   このモードでは、HWPlugin_GetLatencySamples は 0 を返すこと
//   （書き込みは次の HWPlugin_Render の先頭から反映されるため）。
// HWPlugin_Render:
//   開いている全デバイスの音を混ぜ、frames フレーム分を out へ書く。
//   out は L/R インターリーブの float（frames * 2 要素）。1.0 = フルスケール。
//   [-1, 1] に収める処理（クリップ）はしない。複数のプラグインの音を足した後で
//   FITOM が行う。部位ごとのゲイン・マスターボリューム・pan は、音声デバイスへ
//   出すときと同じく掛けること。
//   呼び出しより前に行った HWPlugin_Write / HWPlugin_WriteBlock は、
//   描画する最初のフレームから反映すること。
//   HWPlugin_InitOffline で初期化していなければ HW_ERR_INVALID_ARG。
//   FITOM は HWPlugin_Write と同じスレッドから、交互に呼ぶ（同時には呼ばない）。
FITOM_HWP_API HWResult    FITOM_HWP_CALL HWPlugin_InitOffline(
    const char* profile_path, uint32_t sample_rate);
FITOM_HWP_API HWResult    FITOM_HWP_CALL HWPlugin_Render(
    float* out, uint32_t frames);

#ifdef __cplusplus
}
#endif
