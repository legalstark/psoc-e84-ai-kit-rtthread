# Third-party notices

顶层 Apache-2.0 仅覆盖项目贡献者原创或明确以该许可证提供的修改。以下组件
继续服从各自许可证；本文件是工程清单，不替代对应许可证正文或法律意见。

| 组件 | 来源 | 许可证/发布策略 |
|---|---|---|
| RT-Thread 5.0.2 | Edgi-Talk BSP / RT-Thread | Apache-2.0；源码位于仓库 `rt-thread/` |
| AI Kit target BSP | Infineon | 仓库内保留其 Apache-2.0 LICENSE |
| LVGL 9.2 | LVGL | MIT；源码位于仓库 `libraries/` |
| CherryUSB 1.6.0 | CherryUSB | Apache-2.0；源码位于仓库 `libraries/` |
| Infineon PDL/HAL/Device Support | Infineon | 服从组件随附条款；源码位于仓库 `libraries/` |
| WHD source | Infineon | 服从 WHD 随附 LICENSE；源码位于仓库 `libraries/` |
| CYW55513 firmware/CLM | Infineon | Permissive Binary License 1.0；必须保留许可证和免责声明 |
| SEGGER emUSB-Host 2.3.0 | Infineon/SEGGER | Infineon EULA 明确限制再分发；不提交本仓库，由脚本从官方仓库取得 |
| Secure M33 image | Infineon AI Kit project | 不提交本仓库；由用户从官方工程构建后提供 |
| CoreMark | EEMBC | 服从组件随附 LICENSE.md |
| TFLM Person Detection | TensorFlow Lite Micro | Apache-2.0；许可证位于 `cm55/licenses/` |

下列本地研究资产不属于公开源码树：

```text
TEST_MODEL_int8x8.tflite
TEST_MODEL_sample0.bin
MNIST_U55_int8x8.tflite
MNIST_U55_samples10.bin
```

这些历史研究资产不会出现在公开仓库、构建配置或发布镜像中。
