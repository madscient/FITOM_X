# legacy/

FITOM_X の旧実装のソースコードです。**ビルド対象ではありません**(どの
CMakeLists.txtからも`add_subdirectory`されていません)。

新しいチップドライバ実装(`core/src/*_new.cpp`)・新しいMIDI処理
(`core/src/MidiCh.cpp`/`CFITOM.cpp`)・新しいパッチ管理
(`core/src/PatchManager.cpp`)への移行時に、旧実装の挙動を参照するため
に残しています。新規の開発でこのディレクトリのコードを直接使う・
参照することは想定していません。

- `src/` : 旧チップドライバ・旧MIDI処理・旧パッチ管理等の実装(`.cpp`/`.h`)
- `include/fitom/` : `core/include/fitom/`と同名だが中身が異なる、
  旧実装専用のヘッダ(`core/src/`側の同名ファイルと内容が重複しているため、
  ここに集約しています)
