cmd_drivers/input/built-in.o :=  /opt/arm-2008q1/bin/arm-none-linux-gnueabi-ld -EL    -r -o drivers/input/built-in.o drivers/input/input-core.o drivers/input/evdev.o drivers/input/keyboard/built-in.o drivers/input/touchscreen/built-in.o drivers/input/misc/built-in.o drivers/input/keyreset.o drivers/input/ecompass/built-in.o 
