# Wi-Fi panel for ZETA.  Build on ZETA:   make        Install:  make install
CXX      = c++
CXXFLAGS = -O1 -Wall -Wno-multichar -Wno-ctor-dtor-privacy -ffor-scope
LIBS     = -lbe

WifiPanel: WifiPanel.cpp wificonf.h
	$(CXX) $(CXXFLAGS) -o WifiPanel WifiPanel.cpp $(LIBS)

# copies the panel next to the daemon and puts a link on the Desktop
install: WifiPanel
	mkdir -p /boot/home/ZetaWifi
	cp WifiPanel /boot/home/ZetaWifi/WifiPanel
	ln -sf /boot/home/ZetaWifi/WifiPanel "/boot/home/Desktop/Wi-Fi Panel"

clean:
	rm -f WifiPanel test_wificonf

# the config-editing logic has no Be dependencies: it can be tested on any machine
test:
	cc -Wall -o test_wificonf test_wificonf.c && ./test_wificonf
