import os
from SCons.Script import ARGUMENTS

# toolchains options
ARCH='arm'
CPU='cortex-m55'
CROSS_TOOL='gcc'

# bsp lib config
BSP_LIBRARY_TYPE = None

if os.getenv('RTT_CC'):
    CROSS_TOOL = os.getenv('RTT_CC')
if os.getenv('RTT_ROOT'):
    RTT_ROOT = os.getenv('RTT_ROOT')

# cross_tool provides the cross compiler
# EXEC_PATH is the compiler execute path, for example, CodeSourcery, Keil MDK, IAR
if  CROSS_TOOL == 'gcc':
    PLATFORM    = 'gcc'
    EXEC_PATH   = os.getenv('RTT_EXEC_PATH', '')
elif CROSS_TOOL == 'keil':
    PLATFORM    = 'armclang'
    EXEC_PATH   = os.getenv('RTT_EXEC_PATH', '')
elif CROSS_TOOL == 'iar':
    PLATFORM = 'iccarm'
    EXEC_PATH   = os.getenv('RTT_EXEC_PATH', '')

BUILD = ARGUMENTS.get('BUILD', 'debug').lower()
if BUILD not in ('debug', 'release'):
    raise ValueError("BUILD must be 'debug' or 'release'")

if PLATFORM == 'gcc':
    # toolchains
    PREFIX = 'arm-none-eabi-'
    CC = PREFIX + 'gcc'
    AS = PREFIX + 'gcc'
    AR = PREFIX + 'ar'
    CXX = PREFIX + 'g++'
    LINK = PREFIX + 'gcc'
    TARGET_EXT = 'elf'
    SIZE = PREFIX + 'size'
    OBJDUMP = PREFIX + 'objdump'
    OBJCPY = PREFIX + 'objcopy'

    DEVICE = ' -mcpu=cortex-m55 -mthumb -mfpu=auto --specs=nano.specs -mfloat-abi=hard -ffunction-sections -fdata-sections -nostartfiles'
    CFLAGS = DEVICE + ' -g -Wall -pipe'
    AFLAGS = ' -c' + DEVICE + ' -x assembler-with-cpp -Wa,-mimplicit-it=thumb '
    LFLAGS = DEVICE + ' -Wl,--gc-sections,-Map=rtthread.map,-cref,-u,Reset_Handler'
    LFLAGS += ' -L libs/TARGET_APP_KIT_PSE84_AI/config/GeneratedSource'
    LFLAGS += ' -T board/linker_scripts/link.ld'

    CPATH = ''
    LPATH = ''

    if BUILD == 'debug':
        # CFLAGS += ' -O3 -gdwarf-2 -g'
        # AFLAGS += ' -gdwarf-2'
        CFLAGS += ' -g -O0'
        AFLAGS += ' -g'
    elif BUILD == 'release':
        CFLAGS += ' -O2 -DAI_KIT_BUILD_RELEASE=1'

    CXXFLAGS = CFLAGS 

    POST_ACTION = OBJCPY + ' -O ihex $TARGET rtthread.hex\n' + SIZE + ' $TARGET \n'

elif PLATFORM == 'armclang':
    # toolchains
    CC = 'armclang'
    CXX = 'armclang'
    AS = 'armasm'
    AR = 'armar'
    LINK = 'armlink'
    TARGET_EXT = 'axf'

    DEVICE = ' --cpu Cortex-M55 '
    CFLAGS = ' --target=arm-arm-none-eabi -mcpu=cortex-m55 '
    CFLAGS += ' -mfloat-abi=hard -c -fno-rtti -funsigned-char -fshort-enums -fshort-wchar '
    CFLAGS += ' -gdwarf-3 -ffunction-sections '
    AFLAGS = DEVICE + ' --apcs=interwork '
    LFLAGS = DEVICE + ' --info sizes --info totals --info unused --info veneers '
    LFLAGS += ' --list rt-thread.map '
    LFLAGS += r' --strict --scatter "board\linker_scripts\link.sct" '
    CFLAGS += ' -I' + EXEC_PATH + '/ARM/ARMCLANG/include'
    LFLAGS += ' --libpath=' + EXEC_PATH + '/ARM/ARMCLANG/lib'

    EXEC_PATH += '/ARM/ARMCLANG/bin/'

    if BUILD == 'debug':
        CFLAGS += ' -g -O1' # armclang recommend
        AFLAGS += ' -g'
    elif BUILD == 'release':
        CFLAGS += ' -O2'
        
    CXXFLAGS = CFLAGS
    CFLAGS += ' -std=c99'

    POST_ACTION = 'fromelf --bin $TARGET --output rtthread.bin \nfromelf -z $TARGET'

def dist_handle(BSP_ROOT, dist_dir):
    import sys
    cwd_path = os.getcwd()
    sys.path.append(os.path.join(os.path.dirname(BSP_ROOT), 'tools'))
    from sdk_dist import dist_do_building
    dist_do_building(BSP_ROOT, dist_dir)
