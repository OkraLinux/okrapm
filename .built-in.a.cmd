savedcmd_okrapm/built-in.a := rm -f okrapm/built-in.a;  printf "okrapm/%s " src/main.o src/handler.o src/utils.o | xargs ar cDPrST okrapm/built-in.a
