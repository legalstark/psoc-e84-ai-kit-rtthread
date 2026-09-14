import os

import rtconfig
from building import *

cwd = GetCurrentDir()
configuration = 'CONFIG_Debug' if rtconfig.BUILD == 'debug' else 'CONFIG_Release'
config_root = os.path.join(cwd, 'USBH', 'COMPONENT_USBH_BASE',
                           'COMPONENT_PSE84', configuration)
library_dir = os.path.join(config_root, 'COMPONENT_CM55',
                           'COMPONENT_HARDFP', 'TOOLCHAIN_GCC_ARM')
src = [
    'OS/usbh_os_abs_rtos.c',
    'export/Config/usbh_config_io.c',
    'export/Config/COMPONENT_PSE84/usbh_config.c',
]
path = [cwd + '/USBH', cwd + '/USBH/COMPONENT_PSE84', config_root]
group = DefineGroup('emUSB Host', src,
                    depend=['BSP_USB_ROLE_HOST_UVC'],
                    CPPPATH=path,
                    CPPDEFINES=['USBH_DISABLE_STANDARD_OUTPUT=1'],
                    LIBPATH=[library_dir],
                    LIBS=[File(os.path.join(library_dir, 'gcc.a'))])
Return('group')
