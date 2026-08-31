CXX       = g++
PKGCONFIG = pkg-config
CXXFLAGS  = -Wall -Wextra -O2 -std=c++20 -pthread $(shell $(PKGCONFIG) --cflags dbus-1 libcrypto)
LDFLAGS   = -pthread $(shell $(PKGCONFIG) --libs dbus-1 libcrypto)
TARGET    = pi-bluetooth-configuration
SRCDIR    = src

all: $(TARGET)

$(TARGET): $(SRCDIR)/main.cpp $(SRCDIR)/config.hpp $(SRCDIR)/eth_control.hpp $(SRCDIR)/gatt_server.hpp $(SRCDIR)/wifi_control.hpp $(SRCDIR)/subprocess.hpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCDIR)/main.cpp $(LDFLAGS)

install: all
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/bin/
	install -d $(DESTDIR)/etc/pi-bluetooth-configuration
	install -m 644 config.ini $(DESTDIR)/etc/pi-bluetooth-configuration/config.ini
	install -d $(DESTDIR)/etc/init.d
	install -m 755 openrc/pi-bluetooth-configuration.initd $(DESTDIR)/etc/init.d/pi-bluetooth-configuration

clean:
	rm -f $(TARGET)
