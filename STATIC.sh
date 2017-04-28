#!/bin/sh
export CXXFLAGS='-std=c++11 -O3 -march=core2'
if test $# -gt 0; then
	# If any arguments are given, distclean
	./waf distclean
	./waf configure --enable-static --disable-gtk --disable-tests --disable-examples --disable-nsc -d release
	./waf 
fi

BASEDIR="$PWD"

for scenario in association nosaturated
do
	cd "$BASEDIR/scenarios/$scenario"

	SRC="$scenario.cc"
	OBJ="$scenario.o"
	BIN="$scenario"
	LIBS="-lns3-dev-antenna -lns3-dev-spectrum -lns3-dev-wifi -lns3-dev-propagation -lns3-dev-mobility -lns3-dev-energy -lns3-dev-applications -lns3-dev-internet -lns3-dev-bridge -lns3-dev-mpi -lns3-dev-network -lns3-dev-stats -lns3-dev-core"

	if test "z$CXX" = "zicpc"; then
		icpc -std=c++11 -c -o "$OBJ" -I /s/ls2/sw/gsl/2.1/include/ -I "$BASEDIR/build/" "$SRC"
		icpc -static -L "$BASEDIR/build" -L "/s/ls2/sw/gsl/2.1/lib/" -pthread "$OBJ" -o "$BIN" -Wl,--whole-archive $LIBS -Wl,--no-whole-archive -lrt -lgsl -lgslcblas -lsqlite3
	else
		g++ -std=c++11 -c -o "$OBJ" -I "$BASEDIR/build" "$SRC"
		g++ -static -o "$BIN" "$OBJ" -Wl,--whole-archive -L "$BASEDIR/build" $LIBS -Wl,--no-whole-archive -lgsl -lgslcblas -lm -ldl -pthread -lsqlite3
	fi

#	g++  -g -static-libstdc++ -pthread scratch/${target}/${target}.cc.*.o -o ${target} -Wl,-Bstatic,--whole-archive -L. \
#	-lunwind -ltcmalloc \
#-lns3-dev-lr-wpan -lns3-dev-spectrum -lns3-dev-antenna -lns3-dev-aodv -lns3-dev-olsr -lns3-dev-applications -lns3-dev-csma-layout -lns3-dev-dsdv -lns3-dev-dsr -lns3-dev-flow-monitor -lns3-dev-nix-vector-routing -lns3-dev-point-to-point-layout -lns3-dev-tap-bridge -lns3-dev-internet -lns3-dev-bridge -lns3-dev-point-to-point -lns3-dev-mpi -lns3-dev-uan -lns3-dev-energy -lns3-dev-wifi -lns3-dev-buildings -lns3-dev-propagation -lns3-dev-mobility -lns3-dev-config-store -lns3-dev-csma -lns3-dev-fd-net-device -lns3-dev-test -lns3-dev-virtual-net-device -lns3-dev-topology-read -lns3-dev-network -lns3-dev-stats -lns3-dev-core -lgsl -lgslcblas $(pkg-config --static libxml-2.0 --libs) -lsqlite3 -Wl,-Bdynamic,--no-whole-archive -lrt -lm -ldl -Wl,--no-as-needed,-lprofiler,--as-needed
#	mv ${target} ../scratch/${target}/${target}
done
#-lns3-dev-wimax -lns3-dev-sixlowpan -lns3-dev-lte -lns3-dev-mesh -lns3-dev-wave -lns3-dev-netanim
