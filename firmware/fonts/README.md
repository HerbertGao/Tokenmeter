# 固定 LVGL 字体

随附源文件为 `DejaVuSansMono.ttf`（DejaVu Sans Mono），与硬件参考工程使用的字体一致，没有改字形或重命名字体。SHA-256：

```text
67fa085e9e72a7565ac36cb6a77bdac701e18c7a46d0c15b9b08714a1e1a2042
```

许可原文保存在 `LICENSE-DejaVu.txt`，来源为 DejaVu 官方仓库：
`https://raw.githubusercontent.com/dejavu-fonts/dejavu-fonts/master/LICENSE`。
字体继承 Bitstream Vera 许可；DejaVu 修改部分为 public domain，完整条件以随附原文为准。该许可不代表旧 Clawdmeter 应用或板级参考代码的许可。

从 `firmware/` 手动重现（会覆盖本工程生成的字体，不操作旧项目）：

```sh
sh fonts/generate.sh
```

脚本逐个执行以下命令，未全局安装 npm 包：

```sh
npm exec --yes --package=lv_font_conv@1.5.3 -- lv_font_conv \
  --font DejaVuSansMono.ttf --size 16 --bpp 4 \
  --range '0x20-0x7e,0xb7' --format lvgl --no-compress --no-prefilter \
  --lv-font-name font_16 --output ../src/fonts/font_16.c
```

脚本以自身目录为工作目录；依次生成固定 16/20/24/32/48/96px、4bpp、无压缩的 LVGL C 字体。16–48px 仅含 ASCII 32–126 和 U+00B7；96px 只含空格、`$`、逗号/减号/小数点、数字和 `<`，对应 range `0x20,0x24,0x2c-0x2e,0x30-0x39,0x3c`。每个生成 `.c` 顶部也记录了完整命令；编译直接使用已生成文件，不要求每次构建联网。

TTF 与生成的字库一同保留上述字体许可；本说明不为项目其他代码授予整体许可证。
