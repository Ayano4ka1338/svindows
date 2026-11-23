@echo off
set JAVA_HOME=/usr/lib/jvm/zulu8
set LD_LIBRARY_PATH=/lib:/usr/lib:/usr/lib/jvm/zulu8/lib/amd64:/usr/lib/jvm/zulu8/lib/amd64/jli:/home/user/.minecraft/natives
set LD_PRELOAD=/usr/lib/libGL.so.1
set MESA_LOADER_DRIVER_OVERRIDE=swrast
set GALLIUM_DRIVER=llvmpipe
set LP_NUM_THREADS=2
set ALSOFT_DRIVERS=null
/usr/lib/jvm/zulu8/bin/java -Duser.home=/home/user -Djava.awt.headless=false -Dorg.lwjgl.util.NoChecks=true -Djava.library.path=/home/user/.minecraft/natives -Dorg.lwjgl.librarypath=/home/user/.minecraft/natives -Dnet.java.games.input.librarypath=/home/user/.minecraft/natives -Djinput.useDefaultPlugin=false -Dorg.lwjgl.opengl.Window.nocompiz_lfs=true -Dsun.java2d.xrender=false -Dsun.java2d.pmoffscreen=false -Dos.name=Linux -Dos.version=3.22.0 -Xint -XX:-UseCompressedOops -XX:-UseCompressedClassPointers -Xms32m -Xmx128m -XX:+UseSerialGC -cp /home/user/.minecraft/a1.2.6.jar:/home/user/.minecraft/libraries/* net.minecraft.client.Minecraft
