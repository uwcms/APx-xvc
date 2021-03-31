LDLIBS := $(LDLIBS) -leasymem -l:libledmgr-dl.a -ldl

xvc: xvc.cpp
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	-rm -f xvc *.elf *.gdb *.o *.rpm
