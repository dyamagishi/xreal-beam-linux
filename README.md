# 初代XREAL Beam向けLinux

[本家Linux](https://github.com/torvalds/linux)をフォークし、v7.3を基準に初代XREAL Beamのカーネル変更を積んでいます。Beam Proは対象外です。

- `drivers/`、`include/`、`sound/`: Beamの映像・USB-C・音声・無線・電源・温度監視に関する変更。
- `beam/*.dts`、`beam/*.conf`: 基板定義と設定fragment。
- [温度制御](beam/THERMAL.md): NTC・ファン・充電制御の仕様。
- `beam/tests/`: カーネルのNTC換算試験。

実機確認は初代Beam1台を対象としたものです。別個体や基板改版での互換性は未確認です。通常動作は `beam/mainline-runtime.conf` と `beam/rk3568-xreal-beam-runtime.dts` を使用します。DTS/configとdriverの内容は移行前r4を維持しています。

bootイメージ生成、個体設定、firmware抽出、rootfs準備、A/B復旧、systemd設定とビルド補助は管理リポジトリ `xreal-beam` の `tools/kernel/` で管理します。このカーネルには含めません。個体情報・firmware・起動イメージも含みません。

Linux標準の説明は [README](README)、ライセンスは [COPYING](COPYING)を参照してください。
