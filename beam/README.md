# Beam向けカーネル差分

本家Linux v7.3を祖先とするカーネル移植です。DTS・config fragment、ドライバーとその試験を保持します。

| 場所 | 内容 |
|---|---|
| `rk3568-xreal-beam-runtime.dts` | 通常動作の基板定義 |
| `rk3568-xreal-beam.dts` | 最小bring-up用の基板定義 |
| `mainline-runtime.conf` | 通常動作の設定fragment |
| `mainline-bringup.conf`、`mainline-*-debug.conf` | bring-up・原因切り分け用設定 |
| [THERMAL.md](THERMAL.md) | NTC・ファン・充電の温度制御仕様 |
| `tests/` | NTC換算の独立referenceによる試験 |

ビルドはarm64 defconfigへ設定fragmentをmergeして行います。DTSは `arch/arm64/boot/dts/rockchip/` と `include/` をinclude pathに設定して生成します。管理側の `tools/kernel/build-runtime.sh` と `build-dtb.sh` に再現手順があります。

個体設定・boot生成・rootfs・init・systemd・A/B復旧は管理側 `tools/kernel/` に分離しています。
