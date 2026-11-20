#include <stdint.h>
#include <stdbool.h>
// 128-bit machine values: vector registers and wide arithmetic.
typedef unsigned __int128 uint128_t;
typedef __int128 int128_t;
uint64_t vm_memset(); // 0x10016a2c
extern const uint64_t __entry_r125;

int32_t cs(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5 /* unused */, uint64_t arg6, uint64_t arg7, uint64_t arg8) {
  uint32_t var_3ac; // sp-940
  uint64_t var_3b8; // sp-952
  uint32_t var_3bc; // sp-956
  uint32_t var_3c0; // sp-960
  uint32_t var_3c4; // sp-964
  uint32_t var_3c8; // sp-968
  uint32_t var_3cc; // sp-972
  uint32_t var_3d0; // sp-976
  uint32_t var_3d4; // sp-980
  uint32_t var_3d8; // sp-984
  uint32_t var_3dc; // sp-988
  uint32_t var_3e0; // sp-992
  uint32_t var_3e4; // sp-996
  uint32_t var_3e8; // sp-1000
  uint32_t var_3ec; // sp-1004
  uint32_t var_3f0; // sp-1008
  uint32_t var_3f4; // sp-1012
  uint32_t var_3f8; // sp-1016
  uint32_t var_3fc; // sp-1020
  uint32_t var_400; // sp-1024
  uint32_t var_404; // sp-1028
  uint64_t var_410; // sp-1040
  uint32_t var_414; // sp-1044
  uint32_t var_418; // sp-1048
  uint32_t var_41c; // sp-1052
  uint32_t var_420; // sp-1056
  uint32_t var_424; // sp-1060
  uint32_t var_428; // sp-1064
  uint32_t* var_430; // sp-1072
  uint32_t* var_438; // sp-1080
  uint32_t* var_440; // sp-1088
  uint32_t* var_448; // sp-1096
  uint32_t* var_450; // sp-1104
  uint32_t* var_458; // sp-1112
  uint32_t* var_460; // sp-1120
  uint32_t* var_468; // sp-1128
  uint32_t* var_470; // sp-1136
  uint32_t* var_478; // sp-1144
  uint32_t* var_480; // sp-1152
  uint32_t* var_488; // sp-1160
  uint32_t* var_490; // sp-1168
  uint32_t* var_498; // sp-1176
  uint32_t* var_4a0; // sp-1184
  uint32_t* var_4a8; // sp-1192
  uint32_t var_4b4; // sp-1204
  uint32_t var_4b8; // sp-1208
  uint32_t var_4bc; // sp-1212
  uint32_t var_4c0; // sp-1216
  uint32_t var_4c4; // sp-1220
  uint32_t var_4c8; // sp-1224
  uint32_t var_4cc; // sp-1228
  uint32_t var_4d0; // sp-1232
  uint32_t var_4d4; // sp-1236
  uint32_t var_4d8; // sp-1240
  uint32_t var_4dc; // sp-1244
  uint32_t var_4e0; // sp-1248
  uint32_t var_4e4; // sp-1252
  uint32_t var_4e8; // sp-1256
  uint8_t var_4e9; // sp-1257
  uint8_t var_4ea; // sp-1258
  uint8_t var_4eb; // sp-1259
  uint8_t var_4ec; // sp-1260
  uint8_t var_4ed; // sp-1261
  uint8_t var_4ee; // sp-1262
  uint8_t var_4ef; // sp-1263
  uint8_t var_4f0; // sp-1264
  uint8_t var_4f1; // sp-1265
  uint8_t var_4f2; // sp-1266
  uint8_t var_4f3; // sp-1267
  uint64_t var_500; // sp-1280
  uint64_t var_508; // sp-1288
  uint8_t* var_510; // sp-1296
  uint8_t* var_518; // sp-1304
  uint8_t* var_520; // sp-1312
  uint8_t* var_528; // sp-1320
  uint8_t* var_530; // sp-1328
  uint8_t* var_538; // sp-1336
  uint8_t* var_540; // sp-1344
  uint8_t* var_548; // sp-1352
  uint8_t* var_550; // sp-1360
  uint8_t* var_558; // sp-1368
  uint8_t* var_560; // sp-1376
  uint8_t* var_568; // sp-1384
  uint8_t* var_570; // sp-1392
  uint8_t* var_578; // sp-1400
  uint8_t* var_580; // sp-1408
  uint8_t* var_588; // sp-1416
  uint8_t* var_590; // sp-1424
  uint8_t* var_598; // sp-1432
  uint8_t* var_5a0; // sp-1440
  uint8_t* var_5a8; // sp-1448
  uint8_t* var_5b0; // sp-1456
  uint8_t* var_5b8; // sp-1464
  uint8_t* var_5c0; // sp-1472
  uint8_t* var_5c8; // sp-1480
  uint8_t* var_5d0; // sp-1488
  uint8_t* var_5d8; // sp-1496
  uint8_t* var_5e0; // sp-1504
  uint8_t* var_5e8; // sp-1512
  uint8_t* var_5f0; // sp-1520
  uint8_t* var_5f8; // sp-1528
  uint8_t* var_600; // sp-1536
  uint8_t* var_608; // sp-1544
  uint8_t* var_610; // sp-1552
  uint8_t* var_618; // sp-1560
  uint8_t* var_620; // sp-1568
  uint8_t* var_628; // sp-1576
  uint8_t* var_630; // sp-1584
  uint8_t* var_638; // sp-1592
  uint8_t* var_640; // sp-1600
  uint8_t* var_648; // sp-1608
  uint8_t* var_650; // sp-1616
  uint8_t* var_658; // sp-1624
  uint8_t* var_660; // sp-1632
  uint8_t* var_668; // sp-1640
  uint8_t* var_670; // sp-1648
  uint8_t* var_678; // sp-1656
  uint8_t* var_680; // sp-1664
  uint8_t* var_688; // sp-1672
  uint8_t* var_690; // sp-1680
  uint8_t* var_698; // sp-1688
  uint8_t* var_6a0; // sp-1696
  uint8_t* var_6a8; // sp-1704
  uint8_t* var_6b0; // sp-1712
  uint8_t* var_6b8; // sp-1720
  uint8_t* var_6c0; // sp-1728
  uint8_t* var_6c8; // sp-1736
  uint8_t* var_6d0; // sp-1744
  uint8_t* var_6d8; // sp-1752
  uint8_t* var_6e0; // sp-1760
  uint8_t* var_6e8; // sp-1768
  uint8_t* var_6f0; // sp-1776
  uint8_t* var_6f8; // sp-1784
  uint8_t* var_700; // sp-1792
  uint32_t* var_708; // sp-1800
  uint32_t* var_710; // sp-1808
  uint32_t* var_718; // sp-1816
  uint32_t* var_720; // sp-1824
  uint64_t* var_728; // sp-1832
  uint32_t* var_730; // sp-1840
  uint8_t* var_738; // sp-1848
  uint8_t* var_740; // sp-1856
  uint32_t var_744; // sp-1860
  uint32_t var_748; // sp-1864
  uint32_t var_74c; // sp-1868
  uint32_t var_750; // sp-1872
  uint32_t var_754; // sp-1876
  uint32_t var_758; // sp-1880
  uint32_t var_75c; // sp-1884
  uint32_t var_760; // sp-1888
  uint32_t var_764; // sp-1892
  uint32_t var_768; // sp-1896
  uint32_t var_76c; // sp-1900
  uint32_t var_770; // sp-1904
  uint32_t var_774; // sp-1908
  uint32_t var_778; // sp-1912
  uint32_t var_77c; // sp-1916
  uint32_t var_780; // sp-1920
  uint32_t var_784; // sp-1924
  uint32_t var_788; // sp-1928
  uint32_t var_78c; // sp-1932
  uint32_t var_790; // sp-1936
  uint64_t var_798; // sp-1944
  uint32_t var_79c; // sp-1948
  uint32_t var_7a0; // sp-1952
  uint32_t var_7a4; // sp-1956
  uint32_t var_7a8; // sp-1960
  uint32_t var_7ac; // sp-1964
  uint32_t var_7b0; // sp-1968
  uint32_t var_7b4; // sp-1972
  uint32_t var_7b8; // sp-1976
  uint32_t var_7bc; // sp-1980
  uint32_t var_7c0; // sp-1984
  uint32_t var_7c4; // sp-1988
  uint8_t* var_7d0; // sp-2000
  uint8_t* var_7d8; // sp-2008
  uint8_t* var_7e0; // sp-2016
  uint8_t* var_7e8; // sp-2024
  uint8_t* var_7f0; // sp-2032
  uint8_t* var_7f8; // sp-2040
  uint8_t* var_800; // sp-2048
  uint8_t* var_808; // sp-2056
  uint8_t* var_810; // sp-2064
  uint8_t* var_818; // sp-2072
  uint8_t* var_820; // sp-2080
  uint8_t* var_828; // sp-2088
  uint8_t* var_830; // sp-2096
  uint8_t* var_838; // sp-2104
  uint8_t* var_840; // sp-2112
  uint8_t* var_848; // sp-2120
  uint8_t* var_850; // sp-2128
  uint8_t* var_858; // sp-2136
  uint8_t* var_860; // sp-2144
  uint8_t* var_868; // sp-2152
  uint8_t* var_870; // sp-2160
  uint8_t* var_878; // sp-2168
  uint8_t* var_880; // sp-2176
  uint8_t* var_888; // sp-2184
  uint8_t* var_890; // sp-2192
  uint8_t* var_898; // sp-2200
  uint8_t* var_8a0; // sp-2208
  uint8_t* var_8a8; // sp-2216
  uint8_t* var_8b0; // sp-2224
  uint8_t* var_8b8; // sp-2232
  uint8_t* var_8c0; // sp-2240
  uint8_t* var_8c8; // sp-2248
  uint8_t* var_8d0; // sp-2256
  uint8_t* var_8d8; // sp-2264
  uint8_t* var_8e0; // sp-2272
  uint8_t* var_8e8; // sp-2280
  uint8_t* var_8f0; // sp-2288
  uint8_t* var_8f8; // sp-2296
  uint8_t* var_900; // sp-2304
  uint8_t* var_908; // sp-2312
  uint8_t* var_910; // sp-2320
  uint8_t* var_918; // sp-2328
  uint8_t* var_920; // sp-2336
  uint8_t* var_928; // sp-2344
  uint8_t* var_930; // sp-2352
  uint8_t* var_938; // sp-2360
  uint8_t* var_940; // sp-2368
  uint8_t* var_948; // sp-2376
  uint8_t* var_950; // sp-2384
  uint8_t* var_958; // sp-2392
  uint8_t* var_960; // sp-2400
  uint8_t* var_968; // sp-2408
  uint8_t* var_970; // sp-2416
  uint8_t* var_978; // sp-2424
  uint8_t* var_980; // sp-2432
  uint8_t* var_988; // sp-2440
  uint8_t* var_990; // sp-2448
  uint8_t* var_998; // sp-2456
  uint8_t* var_9a0; // sp-2464
  uint8_t* var_9a8; // sp-2472
  uint8_t* var_9b0; // sp-2480
  uint8_t* var_9b8; // sp-2488
  uint8_t* var_9c0; // sp-2496
  uint32_t* var_9c8; // sp-2504
  uint32_t* var_9d0; // sp-2512
  uint32_t* var_9d8; // sp-2520
  uint32_t* var_9e0; // sp-2528
  uint32_t* var_9e8; // sp-2536
  uint32_t* var_9f0; // sp-2544
  uint32_t* var_9f8; // sp-2552
  uint32_t* var_a00; // sp-2560
  uint64_t* var_a08; // sp-2568
  uint32_t* var_a10; // sp-2576
  uint8_t* var_a18; // sp-2584
  uint8_t* var_a20; // sp-2592
  uint8_t var_a21; // sp-2593
  uint8_t* var_a30; // sp-2608
  uint8_t var_a31; // sp-2609
  uint8_t* var_a40; // sp-2624
  uint8_t var_a41; // sp-2625
  uint8_t* var_a50; // sp-2640
  uint8_t var_a51; // sp-2641
  uint8_t* var_a60; // sp-2656
  uint8_t var_a61; // sp-2657
  uint8_t* var_a70; // sp-2672
  uint8_t var_a71; // sp-2673
  uint8_t* var_a80; // sp-2688
  uint8_t var_a81; // sp-2689
  uint8_t* var_a90; // sp-2704
  uint8_t var_a91; // sp-2705
  uint8_t* var_aa0; // sp-2720
  uint8_t var_aa1; // sp-2721
  uint8_t* var_ab0; // sp-2736
  uint8_t var_ab1; // sp-2737
  uint8_t* var_ac0; // sp-2752
  uint8_t var_ac1; // sp-2753
  uint8_t* var_ad0; // sp-2768
  uint8_t var_ad1; // sp-2769
  uint8_t* var_ae0; // sp-2784
  uint8_t var_ae1; // sp-2785
  uint8_t* var_af0; // sp-2800
  uint8_t var_af1; // sp-2801
  uint8_t* var_b00; // sp-2816
  uint8_t var_b01; // sp-2817
  uint8_t* var_b10; // sp-2832
  uint8_t var_b11; // sp-2833
  uint8_t* var_b20; // sp-2848
  uint8_t var_b21; // sp-2849
  uint8_t* var_b30; // sp-2864
  uint8_t var_b31; // sp-2865
  uint8_t* var_b40; // sp-2880
  uint8_t var_b41; // sp-2881
  uint8_t* var_b50; // sp-2896
  uint8_t var_b51; // sp-2897
  uint8_t* var_b60; // sp-2912
  uint8_t var_b61; // sp-2913
  uint8_t* var_b70; // sp-2928
  uint8_t var_b71; // sp-2929
  uint8_t* var_b80; // sp-2944
  uint8_t var_b81; // sp-2945
  uint8_t* var_b90; // sp-2960
  uint8_t var_b91; // sp-2961
  uint8_t* var_ba0; // sp-2976
  uint8_t var_ba1; // sp-2977
  uint8_t* var_bb0; // sp-2992
  uint8_t var_bb1; // sp-2993
  uint8_t* var_bc0; // sp-3008
  uint8_t var_bc1; // sp-3009
  uint8_t* var_bd0; // sp-3024
  uint8_t var_bd1; // sp-3025
  uint8_t* var_be0; // sp-3040
  uint8_t var_be1; // sp-3041
  uint8_t* var_bf0; // sp-3056
  uint8_t var_bf1; // sp-3057
  uint8_t* var_c00; // sp-3072
  uint8_t var_c01; // sp-3073
  uint8_t var_c02; // sp-3074
  uint32_t var_c08; // sp-3080
  uint32_t var_c0c; // sp-3084
  uint32_t var_c10; // sp-3088
  uint32_t var_c14; // sp-3092
  uint32_t var_c18; // sp-3096
  uint32_t var_c1c; // sp-3100
  uint32_t var_c20; // sp-3104
  uint32_t var_c24; // sp-3108
  uint32_t var_c28; // sp-3112
  uint32_t var_c2c; // sp-3116
  uint32_t var_c30; // sp-3120
  uint32_t var_c34; // sp-3124
  uint32_t var_c38; // sp-3128
  uint32_t var_c3c; // sp-3132
  uint32_t var_c40; // sp-3136
  uint32_t var_c44; // sp-3140
  uint32_t var_c48; // sp-3144
  uint32_t var_c4c; // sp-3148
  uint32_t var_c50; // sp-3152
  uint32_t var_c54; // sp-3156
  uint64_t var_c60; // sp-3168
  uint32_t var_c64; // sp-3172
  uint32_t var_c68; // sp-3176
  uint32_t var_c6c; // sp-3180
  uint32_t var_c70; // sp-3184
  uint32_t var_c74; // sp-3188
  uint32_t var_c78; // sp-3192
  uint32_t var_c7c; // sp-3196
  uint32_t var_c80; // sp-3200
  uint32_t var_c84; // sp-3204
  uint32_t var_c88; // sp-3208
  uint32_t var_c8c; // sp-3212
  uint32_t* var_c98; // sp-3224
  uint8_t* var_ca0; // sp-3232
  uint8_t* var_ca8; // sp-3240
  uint8_t* var_cb0; // sp-3248
  uint8_t* var_cb8; // sp-3256
  uint32_t* var_cc0; // sp-3264
  uint8_t* var_cc8; // sp-3272
  uint8_t* var_cd0; // sp-3280
  uint8_t* var_cd8; // sp-3288
  uint8_t* var_ce0; // sp-3296
  uint32_t* var_ce8; // sp-3304
  uint8_t* var_cf0; // sp-3312
  uint8_t* var_cf8; // sp-3320
  uint8_t* var_d00; // sp-3328
  uint8_t* var_d08; // sp-3336
  uint32_t* var_d10; // sp-3344
  uint8_t* var_d18; // sp-3352
  uint8_t* var_d20; // sp-3360
  uint8_t* var_d28; // sp-3368
  uint8_t* var_d30; // sp-3376
  uint32_t* var_d38; // sp-3384
  uint8_t* var_d40; // sp-3392
  uint8_t* var_d48; // sp-3400
  uint8_t* var_d50; // sp-3408
  uint8_t* var_d58; // sp-3416
  uint32_t* var_d60; // sp-3424
  uint8_t* var_d68; // sp-3432
  uint8_t* var_d70; // sp-3440
  uint8_t* var_d78; // sp-3448
  uint8_t* var_d80; // sp-3456
  uint32_t* var_d88; // sp-3464
  uint8_t* var_d90; // sp-3472
  uint8_t* var_d98; // sp-3480
  uint8_t* var_da0; // sp-3488
  uint8_t* var_da8; // sp-3496
  uint32_t* var_db0; // sp-3504
  uint8_t* var_db8; // sp-3512
  uint8_t* var_dc0; // sp-3520
  uint8_t* var_dc8; // sp-3528
  uint8_t* var_dd0; // sp-3536
  uint32_t* var_dd8; // sp-3544
  uint8_t* var_de0; // sp-3552
  uint8_t* var_de8; // sp-3560
  uint8_t* var_df0; // sp-3568
  uint8_t* var_df8; // sp-3576
  uint32_t* var_e00; // sp-3584
  uint8_t* var_e08; // sp-3592
  uint8_t* var_e10; // sp-3600
  uint8_t* var_e18; // sp-3608
  uint8_t* var_e20; // sp-3616
  uint32_t* var_e28; // sp-3624
  uint8_t* var_e30; // sp-3632
  uint8_t* var_e38; // sp-3640
  uint8_t* var_e40; // sp-3648
  uint8_t* var_e48; // sp-3656
  uint32_t* var_e50; // sp-3664
  uint8_t* var_e58; // sp-3672
  uint8_t* var_e60; // sp-3680
  uint8_t* var_e68; // sp-3688
  uint8_t* var_e70; // sp-3696
  uint32_t* var_e78; // sp-3704
  uint8_t* var_e80; // sp-3712
  uint8_t* var_e88; // sp-3720
  uint8_t* var_e90; // sp-3728
  uint8_t* var_e98; // sp-3736
  uint32_t* var_ea0; // sp-3744
  uint8_t* var_ea8; // sp-3752
  uint8_t* var_eb0; // sp-3760
  uint8_t* var_eb8; // sp-3768
  uint8_t* var_ec0; // sp-3776
  uint32_t* var_ec8; // sp-3784
  uint8_t* var_ed0; // sp-3792
  uint8_t* var_ed8; // sp-3800
  uint8_t* var_ee0; // sp-3808
  uint8_t* var_ee8; // sp-3816
  uint32_t* var_ef0; // sp-3824
  uint8_t* var_ef8; // sp-3832
  uint8_t* var_f00; // sp-3840
  uint8_t* var_f08; // sp-3848
  uint64_t var_f10; // sp-3856
  uint32_t* var_f18; // sp-3864
  uint32_t* var_f20; // sp-3872
  uint32_t* var_f28; // sp-3880
  uint32_t* var_f30; // sp-3888
  uint32_t* var_f38; // sp-3896
  uint32_t* var_f40; // sp-3904
  uint32_t* var_f48; // sp-3912
  uint32_t* var_f50; // sp-3920
  uint64_t* var_f58; // sp-3928
  uint32_t* var_f60; // sp-3936
  uint8_t* var_f68; // sp-3944
  uint64_t var_f70; // sp-3952
  uint64_t var_f78; // sp-3960
  uint64_t var_f80; // sp-3968
  uint64_t var_f88; // sp-3976
  uint64_t var_f90; // sp-3984
  uint64_t var_f98; // sp-3992
  uint64_t var_fa0; // sp-4000
  uint64_t var_fa8; // sp-4008
  uint64_t var_fb0; // sp-4016
  uint64_t var_fb8; // sp-4024
  uint64_t var_fc0; // sp-4032
  uint64_t var_fc8; // sp-4040
  uint8_t* var_fd0; // sp-4048
  uint8_t* var_fd8; // sp-4056
  uint64_t var_fe0; // sp-4064
  uint64_t var_fe8; // sp-4072
  uint64_t var_ff0; // sp-4080
  uint64_t var_ff8; // sp-4088
  uint64_t var_1000; // sp-4096
  uint64_t var_1008; // sp-4104
  uint8_t* var_1010; // sp-4112
  uint64_t var_1018; // sp-4120
  uint64_t var_1020; // sp-4128
  uint64_t var_1028; // sp-4136
  uint64_t var_1030; // sp-4144
  uint64_t var_1038; // sp-4152
  uint32_t var_1040; // sp-4160
  uint64_t var_1188; // sp-4488
  uint64_t var_1190; // sp-4496
  uint64_t var_1198; // sp-4504
  uint64_t var_11a0; // sp-4512
  uint64_t var_11a8; // sp-4520
  uint32_t var_11b0; // sp-4528
  uint64_t var_11f8; // sp-4600
  uint64_t var_1200; // sp-4608
  uint64_t var_1208; // sp-4616
  uint32_t var_1210; // sp-4624
  uint8_t var_1251; // sp-4689
  uint8_t var_1252; // sp-4690
  uint8_t var_1253; // sp-4691
  uint8_t var_1254; // sp-4692
  uint8_t var_1255; // sp-4693
  uint8_t var_1256; // sp-4694
  uint8_t var_1257; // sp-4695
  uint64_t var_1258; // sp-4696
  uint8_t var_1259; // sp-4697
  uint8_t var_125a; // sp-4698
  uint8_t var_125b; // sp-4699
  uint8_t var_125c; // sp-4700
  uint8_t var_125d; // sp-4701
  uint8_t var_125e; // sp-4702
  uint8_t var_125f; // sp-4703
  uint64_t var_1260; // sp-4704
  uint8_t var_1261; // sp-4705
  uint8_t var_1262; // sp-4706
  uint8_t var_1263; // sp-4707
  uint8_t var_1264; // sp-4708
  uint8_t var_1265; // sp-4709
  uint8_t var_1266; // sp-4710
  uint8_t var_1267; // sp-4711
  uint64_t var_1268; // sp-4712
  uint8_t var_1269; // sp-4713
  uint8_t var_126a; // sp-4714
  uint8_t var_126b; // sp-4715
  uint8_t var_126c; // sp-4716
  uint8_t var_126d; // sp-4717
  uint8_t var_126e; // sp-4718
  uint8_t var_126f; // sp-4719
  uint64_t var_1270; // sp-4720
  uint8_t var_1271; // sp-4721
  uint8_t var_1272; // sp-4722
  uint8_t var_1273; // sp-4723
  uint8_t var_1274; // sp-4724
  uint8_t var_1275; // sp-4725
  uint8_t var_1276; // sp-4726
  uint8_t var_1277; // sp-4727
  uint64_t var_1278; // sp-4728
  uint8_t var_1279; // sp-4729
  uint8_t var_127a; // sp-4730
  uint8_t var_127b; // sp-4731
  uint8_t var_127c; // sp-4732
  uint8_t var_127d; // sp-4733
  uint8_t var_127e; // sp-4734
  uint8_t var_127f; // sp-4735
  uint64_t var_1280; // sp-4736
  uint8_t var_1281; // sp-4737
  uint8_t var_1282; // sp-4738
  uint8_t var_1283; // sp-4739
  uint8_t var_1284; // sp-4740
  uint8_t var_1285; // sp-4741
  uint8_t var_1286; // sp-4742
  uint8_t var_1287; // sp-4743
  uint64_t var_1288; // sp-4744
  uint8_t var_1289; // sp-4745
  uint8_t var_128a; // sp-4746
  uint8_t var_128b; // sp-4747
  uint8_t var_128c; // sp-4748
  uint8_t var_128d; // sp-4749
  uint8_t var_128e; // sp-4750
  uint8_t var_128f; // sp-4751
  uint64_t var_1290; // sp-4752
  uint8_t var_1291; // sp-4753
  uint8_t var_1292; // sp-4754
  uint8_t var_1293; // sp-4755
  uint8_t var_1294; // sp-4756
  uint8_t var_1295; // sp-4757
  uint8_t var_1296; // sp-4758
  uint8_t var_1297; // sp-4759
  uint64_t var_1298; // sp-4760
  uint8_t var_1299; // sp-4761
  uint8_t var_129a; // sp-4762
  uint8_t var_129b; // sp-4763
  uint8_t var_129c; // sp-4764
  uint8_t var_129d; // sp-4765
  uint8_t var_129e; // sp-4766
  uint8_t var_129f; // sp-4767
  uint64_t var_12a0; // sp-4768
  uint8_t var_12a1; // sp-4769
  uint8_t var_12a2; // sp-4770
  uint8_t var_12a3; // sp-4771
  uint8_t var_12a4; // sp-4772
  uint8_t var_12a5; // sp-4773
  uint8_t var_12a6; // sp-4774
  uint8_t var_12a7; // sp-4775
  uint64_t var_12a8; // sp-4776
  uint8_t var_12a9; // sp-4777
  uint8_t var_12aa; // sp-4778
  uint8_t var_12ab; // sp-4779
  uint8_t var_12ac; // sp-4780
  uint8_t var_12ad; // sp-4781
  uint8_t var_12ae; // sp-4782
  uint8_t var_12af; // sp-4783
  uint64_t var_12b0; // sp-4784
  uint8_t var_12b1; // sp-4785
  uint8_t var_12b2; // sp-4786
  uint8_t var_12b3; // sp-4787
  uint8_t var_12b4; // sp-4788
  uint8_t var_12b5; // sp-4789
  uint8_t var_12b6; // sp-4790
  uint8_t var_12b7; // sp-4791
  uint64_t var_12b8; // sp-4792
  uint8_t var_12b9; // sp-4793
  uint8_t var_12ba; // sp-4794
  uint8_t var_12bb; // sp-4795
  uint8_t var_12bc; // sp-4796
  uint8_t var_12bd; // sp-4797
  uint8_t var_12be; // sp-4798
  uint8_t var_12bf; // sp-4799
  uint64_t var_12c0; // sp-4800
  uint8_t var_12c1; // sp-4801
  uint8_t var_12c2; // sp-4802
  uint8_t var_12c3; // sp-4803
  uint8_t var_12c4; // sp-4804
  uint8_t var_12c5; // sp-4805
  uint8_t var_12c6; // sp-4806
  uint8_t var_12c7; // sp-4807
  uint64_t var_12c8; // sp-4808
  uint8_t var_12c9; // sp-4809
  uint8_t var_12ca; // sp-4810
  uint8_t var_12cb; // sp-4811
  uint8_t var_12cc; // sp-4812
  uint8_t var_12cd; // sp-4813
  uint8_t var_12ce; // sp-4814
  uint8_t var_12cf; // sp-4815
  uint64_t var_12d0; // sp-4816
  uint8_t var_12d1; // sp-4817
  uint8_t var_12d2; // sp-4818
  uint8_t var_12d3; // sp-4819
  uint8_t var_12d4; // sp-4820
  uint8_t var_12d5; // sp-4821
  uint8_t var_12d6; // sp-4822
  uint8_t var_12d7; // sp-4823
  uint64_t var_12d8; // sp-4824
  uint8_t var_12d9; // sp-4825
  uint8_t var_12da; // sp-4826
  uint8_t var_12db; // sp-4827
  uint8_t var_12dc; // sp-4828
  uint8_t var_12dd; // sp-4829
  uint8_t var_12de; // sp-4830
  uint8_t var_12df; // sp-4831
  uint64_t var_12e0; // sp-4832
  uint8_t var_12e1; // sp-4833
  uint8_t var_12e2; // sp-4834
  uint8_t var_12e3; // sp-4835
  uint8_t var_12e4; // sp-4836
  uint8_t var_12e5; // sp-4837
  uint8_t var_12e6; // sp-4838
  uint8_t var_12e7; // sp-4839
  uint64_t var_12e8; // sp-4840
  uint8_t var_12e9; // sp-4841
  uint8_t var_12ea; // sp-4842
  uint8_t var_12eb; // sp-4843
  uint8_t var_12ec; // sp-4844
  uint8_t var_12ed; // sp-4845
  uint8_t var_12ee; // sp-4846
  uint8_t var_12ef; // sp-4847
  uint64_t var_12f0; // sp-4848
  uint8_t var_12f1; // sp-4849
  uint8_t var_12f2; // sp-4850
  uint8_t var_12f3; // sp-4851
  uint8_t var_12f4; // sp-4852
  uint8_t var_12f5; // sp-4853
  uint8_t var_12f6; // sp-4854
  uint8_t var_12f7; // sp-4855
  uint64_t var_12f8; // sp-4856
  uint8_t var_12f9; // sp-4857
  uint8_t var_12fa; // sp-4858
  uint8_t var_12fb; // sp-4859
  uint8_t var_12fc; // sp-4860
  uint8_t var_12fd; // sp-4861
  uint8_t var_12fe; // sp-4862
  uint8_t var_12ff; // sp-4863
  uint64_t var_1300; // sp-4864
  uint8_t var_1301; // sp-4865
  uint8_t var_1302; // sp-4866
  uint8_t var_1303; // sp-4867
  uint8_t var_1304; // sp-4868
  uint8_t var_1305; // sp-4869
  uint8_t var_1306; // sp-4870
  uint8_t var_1307; // sp-4871
  uint64_t var_1308; // sp-4872
  uint8_t var_1309; // sp-4873
  uint8_t var_130a; // sp-4874
  uint8_t var_130b; // sp-4875
  uint8_t var_130c; // sp-4876
  uint8_t var_130d; // sp-4877
  uint8_t var_130e; // sp-4878
  uint8_t var_130f; // sp-4879
  uint64_t var_1310; // sp-4880
  uint8_t var_1311; // sp-4881
  uint8_t var_1312; // sp-4882
  uint8_t var_1313; // sp-4883
  uint8_t var_1314; // sp-4884
  uint8_t var_1315; // sp-4885
  uint8_t var_1316; // sp-4886
  uint8_t var_1317; // sp-4887
  uint8_t var_1318; // sp-4888
  uint8_t var_1319; // sp-4889
  uint8_t var_131a; // sp-4890
  uint8_t var_131b; // sp-4891
  uint8_t var_131c; // sp-4892
  uint8_t var_131d; // sp-4893
  uint8_t var_131e; // sp-4894
  uint8_t var_131f; // sp-4895
  uint8_t var_1320; // sp-4896
  uint8_t var_1321; // sp-4897
  uint8_t var_1322; // sp-4898
  uint8_t var_1323; // sp-4899
  uint8_t var_1324; // sp-4900
  uint8_t var_1325; // sp-4901
  uint8_t var_1326; // sp-4902
  uint8_t var_1327; // sp-4903
  uint8_t var_1328; // sp-4904
  uint8_t var_1329; // sp-4905
  uint8_t var_132a; // sp-4906
  uint8_t var_132b; // sp-4907
  uint8_t var_132c; // sp-4908
  uint8_t var_1331; // sp-4913
  uint8_t var_1332; // sp-4914
  uint16_t var_1334; // sp-4916
  uint16_t var_1336; // sp-4918
  uint8_t var_1337; // sp-4919
  uint16_t var_1338; // sp-4920
  uint16_t var_133a; // sp-4922
  uint64_t var_1340; // sp-4928
  uint8_t var_1351; // sp-4945
  uint8_t var_1352; // sp-4946
  uint8_t var_1353; // sp-4947
  uint16_t var_1354; // sp-4948
  uint8_t var_1355; // sp-4949
  uint16_t var_1356; // sp-4950
  uint8_t var_1357; // sp-4951
  uint16_t var_1358; // sp-4952
  uint8_t var_1359; // sp-4953
  uint16_t var_135a; // sp-4954
  uint8_t var_135b; // sp-4955
  uint8_t var_135c; // sp-4956
  uint8_t var_135d; // sp-4957
  uint8_t var_135e; // sp-4958
  uint8_t var_135f; // sp-4959
  uint64_t var_1360; // sp-4960
  uint32_t var_1364; // sp-4964
  uint64_t var_1370; // sp-4976
  uint64_t var_1378; // sp-4984
  uint64_t var_1380; // sp-4992
  uint64_t var_1388; // sp-5000
  uint64_t var_1390; // sp-5008
  uint64_t var_1398; // sp-5016
  uint64_t var_13a0; // sp-5024
  uint64_t var_13a8; // sp-5032
  uint64_t var_13b0; // sp-5040
  uint64_t var_13b8; // sp-5048
  uint64_t var_13c0; // sp-5056
  uint64_t var_13c8; // sp-5064
  uint64_t var_13d0; // sp-5072
  uint64_t var_13d8; // sp-5080
  uint32_t var_13dc; // sp-5084
  uint64_t var_13e8; // sp-5096
  uint32_t var_13ec; // sp-5100
  uint64_t var_13f8; // sp-5112
  uint64_t var_1400; // sp-5120
  uint32_t var_1404; // sp-5124
  uint32_t var_1408; // sp-5128
  uint32_t var_140c; // sp-5132
  uint32_t var_1410; // sp-5136
  uint32_t var_1414; // sp-5140
  uint32_t var_1418; // sp-5144
  uint32_t var_141c; // sp-5148
  uint32_t var_1420; // sp-5152
  uint32_t var_1424; // sp-5156
  uint32_t var_1428; // sp-5160
  uint64_t var_1430; // sp-5168
  uint64_t var_1438; // sp-5176
  uint64_t var_1440; // sp-5184
  uint64_t var_1448; // sp-5192
  uint64_t var_1450; // sp-5200
  uint64_t var_1458; // sp-5208
  uint64_t var_1460; // sp-5216
  uint64_t var_1468; // sp-5224
  uint64_t var_1470; // sp-5232
  uint64_t var_1478; // sp-5240
  uint64_t var_1480; // sp-5248
  uint64_t var_1488; // sp-5256
  uint64_t var_1490; // sp-5264
  uint64_t var_1498; // sp-5272
  uint64_t var_14a0; // sp-5280
  uint64_t var_14a8; // sp-5288
  uint64_t var_14b0; // sp-5296
  uint64_t var_14b8; // sp-5304
  uint64_t var_14c0; // sp-5312
  uint64_t var_14c8; // sp-5320
  uint64_t var_14d0; // sp-5328
  uint64_t var_14d8; // sp-5336
  uint64_t var_14e0; // sp-5344
  uint64_t var_14e8; // sp-5352
  uint64_t var_14f0; // sp-5360
  uint64_t var_14f8; // sp-5368
  uint64_t var_1500; // sp-5376
  uint64_t var_1508; // sp-5384
  uint64_t var_1510; // sp-5392
  uint64_t var_1518; // sp-5400
  uint64_t var_1520; // sp-5408
  uint64_t var_1528; // sp-5416
  uint64_t var_1530; // sp-5424
  uint64_t var_1538; // sp-5432
  uint64_t var_1540; // sp-5440
  uint64_t var_1548; // sp-5448
  uint64_t var_1550; // sp-5456
  uint64_t var_1558; // sp-5464
  uint64_t var_1560; // sp-5472
  uint64_t var_1568; // sp-5480
  uint64_t var_1570; // sp-5488
  uint64_t var_1578; // sp-5496
  uint64_t var_1580; // sp-5504
  uint64_t var_1588; // sp-5512
  uint64_t var_1590; // sp-5520
  uint64_t var_1598; // sp-5528
  uint64_t var_15a0; // sp-5536
  uint64_t var_15a8; // sp-5544
  uint64_t var_15b0; // sp-5552
  uint64_t var_15b8; // sp-5560
  uint64_t var_15c0; // sp-5568
  uint64_t var_15c8; // sp-5576
  uint64_t var_15d0; // sp-5584
  uint64_t var_15d8; // sp-5592
  uint64_t var_15e0; // sp-5600
  uint64_t var_15e8; // sp-5608
  uint64_t var_15f0; // sp-5616
  uint64_t var_15f8; // sp-5624
  uint64_t var_1600; // sp-5632
  uint64_t var_1608; // sp-5640
  uint64_t var_1610; // sp-5648
  uint64_t var_1618; // sp-5656
  uint64_t var_1620; // sp-5664
  uint64_t var_1628; // sp-5672
  uint64_t var_1630; // sp-5680
  uint64_t var_1638; // sp-5688
  uint64_t var_1640; // sp-5696
  uint64_t var_1648; // sp-5704
  uint64_t var_1650; // sp-5712
  uint64_t var_1658; // sp-5720
  uint64_t var_1660; // sp-5728
  uint64_t var_1668; // sp-5736
  uint64_t var_1670; // sp-5744
  uint64_t var_1678; // sp-5752
  uint64_t var_1680; // sp-5760
  uint64_t var_1688; // sp-5768
  uint64_t var_1690; // sp-5776
  uint64_t var_1698; // sp-5784
  uint64_t var_16a0; // sp-5792
  uint64_t var_16a8; // sp-5800
  uint32_t var_16ac; // sp-5804
  uint64_t var_16b8; // sp-5816
  uint64_t var_16c0; // sp-5824
  uint64_t var_16c8; // sp-5832
  uint32_t var_16cc; // sp-5836
  uint32_t var_16d0; // sp-5840
  uint32_t var_16d4; // sp-5844
  uint32_t var_16d8; // sp-5848
  uint32_t var_16dc; // sp-5852
  uint32_t var_16e0; // sp-5856
  uint32_t var_16e4; // sp-5860
  uint32_t var_16e8; // sp-5864
  uint32_t var_16ec; // sp-5868
  uint64_t var_16f8; // sp-5880
  uint64_t var_1700; // sp-5888
  uint64_t var_1708; // sp-5896
  uint64_t var_1710; // sp-5904
  uint64_t var_1718; // sp-5912
  uint64_t var_1720; // sp-5920
  uint64_t var_1728; // sp-5928
  uint64_t var_1730; // sp-5936
  uint64_t var_1738; // sp-5944
  uint64_t var_1740; // sp-5952
  uint64_t var_1748; // sp-5960
  uint64_t var_1750; // sp-5968
  uint64_t var_1758; // sp-5976
  uint64_t var_1760; // sp-5984
  uint64_t var_1768; // sp-5992
  uint64_t var_1770; // sp-6000
  uint64_t var_1778; // sp-6008
  uint64_t var_1780; // sp-6016
  uint64_t var_1788; // sp-6024
  uint64_t var_1790; // sp-6032
  uint64_t var_1798; // sp-6040
  uint64_t var_17a0; // sp-6048
  uint64_t var_17a8; // sp-6056
  uint64_t var_17b0; // sp-6064
  uint64_t var_17b8; // sp-6072
  uint64_t var_17c0; // sp-6080
  uint64_t var_17c8; // sp-6088
  uint64_t var_17d0; // sp-6096
  uint64_t var_17d8; // sp-6104
  uint64_t var_17e0; // sp-6112
  uint64_t var_17e8; // sp-6120
  uint64_t var_17f0; // sp-6128
  uint64_t var_17f8; // sp-6136
  uint64_t var_1800; // sp-6144
  uint64_t var_1808; // sp-6152
  uint64_t var_1810; // sp-6160
  uint64_t var_1818; // sp-6168
  uint64_t var_1820; // sp-6176
  uint64_t var_1828; // sp-6184
  uint64_t var_1830; // sp-6192
  uint64_t var_1838; // sp-6200
  uint64_t var_1840; // sp-6208
  uint64_t var_1848; // sp-6216
  uint64_t var_1850; // sp-6224
  uint64_t var_1858; // sp-6232
  uint64_t var_1860; // sp-6240
  uint64_t var_1868; // sp-6248
  uint64_t var_1870; // sp-6256
  uint64_t var_1878; // sp-6264
  uint64_t var_1880; // sp-6272
  uint64_t var_1888; // sp-6280
  uint64_t var_1890; // sp-6288
  uint64_t var_1898; // sp-6296
  uint64_t var_18a0; // sp-6304
  uint64_t var_18a8; // sp-6312
  uint64_t var_18b0; // sp-6320
  uint64_t var_18b8; // sp-6328
  uint64_t var_18c0; // sp-6336
  uint64_t var_18c8; // sp-6344
  uint64_t var_18d0; // sp-6352
  uint64_t var_18d8; // sp-6360
  uint64_t var_18e0; // sp-6368
  uint64_t var_18e8; // sp-6376
  uint64_t var_18f0; // sp-6384
  uint64_t var_18f8; // sp-6392
  uint64_t var_1900; // sp-6400
  uint64_t var_1908; // sp-6408
  uint64_t var_1910; // sp-6416
  uint64_t var_1918; // sp-6424
  uint64_t var_1920; // sp-6432
  uint64_t var_1928; // sp-6440
  uint64_t var_1930; // sp-6448
  uint64_t var_1938; // sp-6456
  uint64_t var_1940; // sp-6464
  uint64_t var_1948; // sp-6472
  uint64_t var_1950; // sp-6480
  uint64_t var_1958; // sp-6488
  uint64_t var_1960; // sp-6496
  uint64_t var_1968; // sp-6504
  uint64_t var_1970; // sp-6512
  uint64_t var_1978; // sp-6520
  uint64_t var_1980; // sp-6528
  uint32_t var_1984; // sp-6532
  uint32_t var_1988; // sp-6536
  uint32_t var_198c; // sp-6540
  uint32_t var_1990; // sp-6544
  uint32_t var_1994; // sp-6548
  uint32_t var_1998; // sp-6552
  uint32_t var_199c; // sp-6556
  uint32_t var_19a0; // sp-6560
  uint32_t var_19a4; // sp-6564
  uint64_t var_19b0; // sp-6576
  uint32_t var_19b4; // sp-6580
  uint64_t var_19c0; // sp-6592
  uint64_t var_19c8; // sp-6600
  uint32_t var_19cc; // sp-6604
  uint32_t var_19d0; // sp-6608
  uint32_t var_19d4; // sp-6612
  uint32_t var_19d8; // sp-6616
  uint32_t var_19dc; // sp-6620
  uint32_t var_19e0; // sp-6624
  uint32_t var_19e4; // sp-6628
  uint32_t var_19e8; // sp-6632
  uint32_t var_19ec; // sp-6636
  uint32_t var_19f0; // sp-6640
  uint64_t var_19f8; // sp-6648
  uint64_t var_1a00; // sp-6656
  uint32_t var_1a04; // sp-6660
  uint32_t var_1a08; // sp-6664
  uint32_t var_1a0c; // sp-6668
  uint32_t var_1a10; // sp-6672
  uint32_t var_1a14; // sp-6676
  uint32_t var_1a18; // sp-6680
  uint32_t var_1a1c; // sp-6684
  uint32_t var_1a20; // sp-6688
  uint32_t var_1a24; // sp-6692
  uint64_t var_1a30; // sp-6704
  uint64_t var_1a38; // sp-6712
  uint32_t var_1a3c; // sp-6716
  uint32_t var_1a40; // sp-6720
  uint32_t var_1a44; // sp-6724
  uint32_t var_1a48; // sp-6728
  uint32_t var_1a4c; // sp-6732
  uint32_t var_1a50; // sp-6736
  uint32_t var_1a54; // sp-6740
  uint32_t var_1a58; // sp-6744
  uint32_t var_1a5c; // sp-6748
  uint32_t var_1a60; // sp-6752
  uint64_t var_1a68; // sp-6760
  uint32_t var_1a6c; // sp-6764
  uint8_t* var_1a78; // sp-6776
  uint32_t var_1a7c; // sp-6780
  uint64_t var_1a88; // sp-6792
  uint64_t var_1a90; // sp-6800
  uint64_t var_1a98; // sp-6808
  uint64_t var_1aa0; // sp-6816
  uint64_t var_1aa8; // sp-6824
  uint64_t var_1ab0; // sp-6832
  uint64_t var_1ab8; // sp-6840
  uint64_t var_1ac0; // sp-6848
  uint64_t var_1ac8; // sp-6856
  uint64_t var_1ad0; // sp-6864
  uint64_t var_1ad8; // sp-6872
  uint64_t var_1ae0; // sp-6880
  uint64_t var_1ae8; // sp-6888
  uint64_t var_1af0; // sp-6896
  uint64_t var_1af8; // sp-6904
  uint64_t var_1b00; // sp-6912
  uint64_t var_1b08; // sp-6920
  uint64_t var_1b10; // sp-6928
  uint64_t var_1b18; // sp-6936
  uint64_t var_1b20; // sp-6944
  uint64_t var_1b28; // sp-6952
  uint64_t var_1b30; // sp-6960
  uint64_t var_1b38; // sp-6968
  uint64_t var_1b40; // sp-6976
  uint64_t var_1b48; // sp-6984
  uint64_t var_1b50; // sp-6992
  uint64_t var_1b58; // sp-7000
  uint64_t var_1b60; // sp-7008
  uint64_t var_1b68; // sp-7016
  uint64_t var_1b70; // sp-7024
  uint64_t var_1b78; // sp-7032
  uint64_t var_1b80; // sp-7040
  uint64_t var_1b88; // sp-7048
  uint64_t var_1b90; // sp-7056
  uint64_t var_1b98; // sp-7064
  uint64_t var_1ba0; // sp-7072
  uint64_t var_1ba8; // sp-7080
  uint64_t var_1bb0; // sp-7088
  uint64_t var_1bb8; // sp-7096
  uint64_t var_1bc0; // sp-7104
  uint64_t var_1bc8; // sp-7112
  uint64_t var_1bd0; // sp-7120
  uint64_t var_1bd8; // sp-7128
  uint64_t var_1be0; // sp-7136
  uint64_t var_1be8; // sp-7144
  uint64_t var_1bf0; // sp-7152
  uint64_t var_1bf8; // sp-7160
  uint64_t var_1c00; // sp-7168
  uint64_t var_1c08; // sp-7176
  uint64_t var_1c10; // sp-7184
  uint64_t var_1c18; // sp-7192
  uint64_t var_1c20; // sp-7200
  uint64_t var_1c28; // sp-7208
  uint64_t var_1c30; // sp-7216
  uint64_t var_1c38; // sp-7224
  uint64_t var_1c40; // sp-7232
  uint64_t var_1c48; // sp-7240
  uint64_t var_1c50; // sp-7248
  uint64_t var_1c58; // sp-7256
  uint64_t var_1c60; // sp-7264
  uint64_t var_1c68; // sp-7272
  uint64_t var_1c70; // sp-7280
  uint64_t var_1c78; // sp-7288
  uint64_t var_1c80; // sp-7296
  uint64_t var_1c88; // sp-7304
  uint64_t var_1c90; // sp-7312
  uint64_t var_1c98; // sp-7320
  uint64_t var_1ca0; // sp-7328
  uint64_t var_1ca8; // sp-7336
  uint64_t var_1cb0; // sp-7344
  uint64_t var_1cb8; // sp-7352
  uint64_t var_1cc0; // sp-7360
  uint64_t var_1cc8; // sp-7368
  uint64_t var_1cd0; // sp-7376
  uint64_t var_1cd8; // sp-7384
  uint64_t var_1ce0; // sp-7392
  uint64_t var_1ce8; // sp-7400
  uint64_t var_1cf0; // sp-7408
  uint64_t var_1cf8; // sp-7416
  uint64_t var_1d00; // sp-7424
  uint64_t* var_1d08; // sp-7432
  uint32_t* var_1d10; // sp-7440
  uint64_t var_1d18; // sp-7448
  uint64_t var_1d20; // sp-7456
  uint64_t var_1d28; // sp-7464
  uint64_t var_1d30; // sp-7472
  uint64_t var_1d38; // sp-7480
  uint64_t var_1d40; // sp-7488
  uint64_t var_1d48; // sp-7496
  uint64_t var_1d50; // sp-7504
  uint64_t var_1d58; // sp-7512
  uint64_t t0, t1, t2, t3, t4, t5, t6, t7, t9, t10, t11, t12;
  uint64_t t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23, t24;
  uint64_t t25, t26, t27, t28, t29, t30, t31, t32, t33, t34, t35, t36;
  uint64_t t37, t38, t39, t104, t109, t110, t111, t112, t113, t114, t115, t116;
  uint64_t t117, t118, t119, t120, t121, t122, t123, t124, t125, t126, t127, t128;
  uint64_t t129, t130, t131, t132, t133, t134, t135, t136, t137, t138, t139, t140;
  uint64_t t141, t142, t143, t144, t145, t146, t147, t148, t149, t150, t151, t152;
  uint64_t t153, t154, t155, t156, t157, t158, t159, t160, t161, t166, t183, t184;
  uint64_t t185, t186, t187, t188, t189, t190, t195, t196, t205, t206, t207, t208;
  uint64_t t209, t210, t211, t212, t213, t214, t215, t216, t217, t218, t219, t220;
  uint64_t t221, t222, t223, t224, t225, t226, t227, t228, t229, t230, t231, t232;
  uint64_t t233, t234, t235, t236, t237, t238, t239, t240, t241, t242, t243, t244;
  uint64_t t245, t246, t247, t248, t249, t250, t251, t252, t253, t254, t255, t256;
  uint64_t t257, t258, t259, t260, t261, t262, t263, t264, t265, t266, t267, t269;
  uint64_t t270, t275, t277, t278, t279, t291, t292, t293, t294, t295, t299, t300;
  uint64_t t301, t302, t303, t304, t305, t315, t316, t317, t318, t319, t320, t321;
  uint64_t t322, t323, t324, t325, t326, t327, t328, t329, t330, t331, t332, t333;
  uint64_t t334, t335, t336, t337, t338, t339, t340, t341, t342, t343, t344, t345;
  uint64_t t346, t347, t348, t349, t350, t351, t352, t353, t354, t355, t356, t357;
  uint64_t t358, t359, t360, t361, t362, t363, t364, t365, t366, t367, t368, t369;
  uint64_t t370, t371, t372, t373, t374, t375, t376, t377, t378, t379, t380, t381;
  uint64_t t382, t383, t384, t385, t386, t387, t388, t389, t390, t391, t392, t393;
  uint64_t t394, t395, t396, t405, t406, t407, t411, t412, t416, t417, t418, t419;
  uint64_t t420, t421, t422, t423, t424, t425, t426, t427, t436, t437, t469, t471;
  uint64_t t472, t473, t474, t475, t476, t486, t487, t488, t489, t490, t491, t492;
  uint64_t t493, t494, t495, t496, t505, t519, t520, t521, t523, t525, t526, t527;
  uint64_t t528, t589, t590, t591, t592, t593, t594, t595, t596, t597, t598, t599;
  uint64_t t600, t601, t602, t603, t604, t605, t606, t607, t608, t609, t610, t611;
  uint64_t t612, t613, t614, t615, t616, t617, t618, t619, t620, t621, t622, t623;
  uint64_t t624, t625, t626, t627, t628, t629, t630, t631, t632, t633, t634, t635;
  uint64_t t636, t637, t638, t639, t640, t641, t642, t643, t644, t645, t646, t663;
  uint64_t t664, t665, t666, t667, t668, t669, t670, t671, t681, t683, t748, t753;
  uint64_t t754, t755, t756, t757, t758, t759, t760, t761, t762, t763, t764, t765;
  uint64_t t766, t767, t768, t769, t770, t771, t772, t773, t774, t775, t776, t777;
  uint64_t t778, t779, t780, t781, t782, t783, t784, t785, t786, t787, t788, t789;
  uint64_t t790, t791, t792, t793, t794, t795, t796, t797, t798, t799, t800, t801;
  uint64_t t802, t803, t804, t805, t822, t823, t824, t825, t834, t840, t841, t873;
  uint64_t t875, t884, t885, t886, t887, t888, t889, t890, t891, t892, t893, t894;
  uint64_t t895, t896, t897, t898, t899, t900, t901, t902, t903, t904, t905, t906;
  uint64_t t907, t908, t909, t910, t911, t912, t913, t914, t916, t917, t918, t919;
  uint64_t t920, t921, t922, t923, _cse1, _cse2, _cse3, _cse4, _cse5, _cse6, _cse7, _cse10;
  uint64_t _cse9, _cse8, _cse12, _cse13, _cse14, _cse11, _cse15, _cse16, _cse17, _cse18, _cse19, _cse20;
  uint64_t _cse23, _cse26, _cse29, _cse32, _cse35, _cse38, _cse41, _cse44, _cse47, _cse50, _cse53, _cse56;
  uint64_t _cse59, _cse62, _cse65, _cse68, _cse69, _cse72, _cse74, _cse75, _cse73, _cse71, _cse70, _cse67;
  uint64_t _cse66, _cse83, _cse82, _cse81, _cse80, _cse79, _cse78, _cse77, _cse76, _cse64, _cse63, _cse89;
  uint64_t _cse88, _cse87, _cse86, _cse85, _cse84, _cse91, _cse90, _cse61, _cse60, _cse95, _cse94, _cse93;
  uint64_t _cse92, _cse97, _cse96, _cse58, _cse57, _cse103, _cse102, _cse101, _cse100, _cse99, _cse98, _cse55;
  uint64_t _cse54, _cse109, _cse108, _cse107, _cse106, _cse105, _cse104, _cse52, _cse51, _cse115, _cse114, _cse113;
  uint64_t _cse112, _cse111, _cse110, _cse49, _cse48, _cse121, _cse120, _cse119, _cse118, _cse117, _cse116, _cse46;
  uint64_t _cse45, _cse128, _cse127, _cse126, _cse129, _cse125, _cse124, _cse123, _cse122, _cse43, _cse42, _cse135;
  uint64_t _cse134, _cse133, _cse132, _cse131, _cse130, _cse40, _cse39, _cse141, _cse140, _cse139, _cse138, _cse137;
  uint64_t _cse136, _cse37, _cse36, _cse147, _cse146, _cse145, _cse144, _cse143, _cse142, _cse34, _cse33, _cse149;
  uint64_t _cse148, _cse153, _cse152, _cse151, _cse150, _cse31, _cse30, _cse155, _cse154, _cse159, _cse158, _cse157;
  uint64_t _cse156, _cse28, _cse27, _cse161, _cse160, _cse165, _cse164, _cse163, _cse162, _cse25, _cse24, _cse167;
  uint64_t _cse166, _cse171, _cse170, _cse169, _cse168, _cse22, _cse21, _cse173, _cse172, _cse174, _cse178, _cse181;
  uint64_t _cse184, _cse187, _cse190, _cse193, _cse196, _cse199, _cse202, _cse205, _cse208, _cse211, _cse214, _cse217;
  uint64_t _cse220, _cse221, _cse224, _cse228, _cse231, _cse232, _cse230, _cse229, _cse227, _cse226, _cse225, _cse223;
  uint64_t _cse222, _cse219, _cse218, _cse240, _cse239, _cse238, _cse237, _cse236, _cse235, _cse234, _cse233, _cse216;
  uint64_t _cse215, _cse243, _cse244, _cse246, _cse245, _cse242, _cse241, _cse248, _cse247, _cse213, _cse212, _cse252;
  uint64_t _cse251, _cse250, _cse249, _cse254, _cse253, _cse210, _cse209, _cse260, _cse259, _cse258, _cse257, _cse256;
  uint64_t _cse255, _cse207, _cse206, _cse266, _cse265, _cse264, _cse263, _cse262, _cse261, _cse204, _cse203, _cse272;
  uint64_t _cse271, _cse270, _cse269, _cse268, _cse267, _cse201, _cse200, _cse278, _cse277, _cse276, _cse275, _cse279;
  uint64_t _cse274, _cse273, _cse280, _cse198, _cse197, _cse286, _cse285, _cse284, _cse283, _cse282, _cse281, _cse195;
  uint64_t _cse194, _cse292, _cse291, _cse290, _cse289, _cse288, _cse287, _cse192, _cse191, _cse298, _cse297, _cse296;
  uint64_t _cse295, _cse294, _cse293, _cse189, _cse188, _cse304, _cse303, _cse302, _cse301, _cse300, _cse299, _cse186;
  uint64_t _cse185, _cse306, _cse305, _cse310, _cse309, _cse308, _cse307, _cse183, _cse182, _cse312, _cse311, _cse316;
  uint64_t _cse315, _cse314, _cse313, _cse180, _cse179, _cse318, _cse317, _cse322, _cse321, _cse320, _cse319, _cse177;
  uint64_t _cse176, _cse326, _cse325, _cse324, _cse323, _cse327, _cse328, _cse329, _cse330, _cse331, _cse333, _cse334;
  uint64_t _cse335, _cse336, _cse337, _cse338, _cse339, _cse340, _cse341, _cse342, _cse343, _cse345, _cse347, _cse346;
  uint64_t _cse344, _cse349, _cse348, _cse350, _cse351, _cse352, _cse353, _cse357, _cse360, _cse363, _cse366, _cse369;
  uint64_t _cse372, _cse375, _cse378, _cse381, _cse384, _cse387, _cse390, _cse393, _cse396, _cse399, _cse402, _cse403;
  uint64_t _cse406, _cse408, _cse409, _cse407, _cse405, _cse404, _cse401, _cse400, _cse417, _cse416, _cse415, _cse414;
  uint64_t _cse413, _cse412, _cse411, _cse410, _cse398, _cse397, _cse423, _cse422, _cse421, _cse420, _cse419, _cse418;
  uint64_t _cse425, _cse424, _cse395, _cse394, _cse429, _cse428, _cse427, _cse426, _cse431, _cse430, _cse392, _cse391;
  uint64_t _cse437, _cse436, _cse435, _cse434, _cse433, _cse432, _cse389, _cse388, _cse443, _cse442, _cse441, _cse440;
  uint64_t _cse439, _cse438, _cse386, _cse385, _cse449, _cse448, _cse447, _cse446, _cse445, _cse444, _cse383, _cse382;
  uint64_t _cse455, _cse454, _cse453, _cse452, _cse451, _cse450, _cse380, _cse379, _cse462, _cse461, _cse460, _cse463;
  uint64_t _cse459, _cse458, _cse457, _cse456, _cse377, _cse376, _cse469, _cse468, _cse467, _cse466, _cse465, _cse464;
  uint64_t _cse374, _cse373, _cse475, _cse474, _cse473, _cse472, _cse471, _cse470, _cse371, _cse370, _cse481, _cse480;
  uint64_t _cse479, _cse478, _cse477, _cse476, _cse368, _cse367, _cse483, _cse482, _cse487, _cse486, _cse485, _cse484;
  uint64_t _cse365, _cse364, _cse489, _cse488, _cse493, _cse492, _cse491, _cse490, _cse362, _cse361, _cse495, _cse494;
  uint64_t _cse499, _cse498, _cse497, _cse496, _cse359, _cse358, _cse501, _cse500, _cse505, _cse504, _cse503, _cse502;
  uint64_t _cse356, _cse355, _cse354, _cse506, _cse509, _cse508, _cse507, _cse511, _cse510, _cse512, _cse513, _cse516;
  uint64_t _cse519, _cse522, _cse525, _cse528, _cse531, _cse534, _cse537, _cse554, _cse556, _cse557, _cse555, _cse558;
  uint64_t _cse553, _cse552, _cse562, _cse565, _cse568, _cse567, _cse566, _cse564, _cse563, _cse561, _cse560, _cse559;
  uint64_t _cse551, _cse550, _cse572, _cse575, _cse578, _cse577, _cse576, _cse574, _cse573, _cse571, _cse570, _cse569;
  uint64_t _cse549, _cse548, _cse581, _cse583, _cse582, _cse580, _cse579, _cse586, _cse585, _cse584, _cse547, _cse546;
  uint64_t _cse590, _cse589, _cse592, _cse591, _cse596, _cse595, _cse594, _cse593, _cse588, _cse587, _cse545, _cse544;
  uint64_t _cse602, _cse601, _cse600, _cse599, _cse598, _cse597, _cse543, _cse542, _cse608, _cse607, _cse606, _cse605;
  uint64_t _cse604, _cse603, _cse541, _cse540, _cse614, _cse613, _cse612, _cse611, _cse610, _cse609, _cse539, _cse538;
  uint64_t _cse620, _cse619, _cse621, _cse618, _cse617, _cse622, _cse616, _cse615, _cse536, _cse535, _cse628, _cse627;
  uint64_t _cse626, _cse625, _cse624, _cse623, _cse533, _cse532, _cse634, _cse633, _cse632, _cse631, _cse630, _cse629;
  uint64_t _cse530, _cse529, _cse640, _cse639, _cse638, _cse637, _cse636, _cse635, _cse527, _cse526, _cse642, _cse641;
  uint64_t _cse646, _cse645, _cse644, _cse643, _cse524, _cse523, _cse648, _cse647, _cse652, _cse651, _cse650, _cse649;
  uint64_t _cse521, _cse520, _cse654, _cse653, _cse658, _cse657, _cse656, _cse655, _cse518, _cse517, _cse660, _cse659;
  uint64_t _cse664, _cse663, _cse662, _cse661, _cse515, _cse514, _cse667, _cse666, _cse665, _cse668, _cse673, _cse676;
  uint64_t _cse679, _cse682, _cse685, _cse688, _cse691, _cse694, _cse697, _cse700, _cse703, _cse706, _cse709, _cse712;
  uint64_t _cse715, _cse718, _cse719, _cse722, _cse724, _cse725, _cse723, _cse721, _cse720, _cse717, _cse716, _cse733;
  uint64_t _cse732, _cse731, _cse730, _cse729, _cse728, _cse727, _cse726, _cse714, _cse713, _cse739, _cse738, _cse737;
  uint64_t _cse736, _cse735, _cse734, _cse741, _cse740, _cse711, _cse710, _cse745, _cse744, _cse743, _cse742, _cse747;
  uint64_t _cse746, _cse708, _cse707, _cse753, _cse752, _cse751, _cse750, _cse749, _cse748, _cse705, _cse704, _cse759;
  uint64_t _cse758, _cse757, _cse756, _cse755, _cse754, _cse702, _cse701, _cse765, _cse764, _cse763, _cse762, _cse761;
  uint64_t _cse760, _cse699, _cse698, _cse771, _cse770, _cse769, _cse768, _cse767, _cse766, _cse696, _cse695, _cse778;
  uint64_t _cse777, _cse776, _cse779, _cse775, _cse774, _cse773, _cse772, _cse693, _cse692, _cse785, _cse784, _cse783;
  uint64_t _cse782, _cse781, _cse780, _cse690, _cse689, _cse791, _cse790, _cse789, _cse788, _cse787, _cse786, _cse687;
  uint64_t _cse686, _cse797, _cse796, _cse795, _cse794, _cse793, _cse792, _cse684, _cse683, _cse799, _cse798, _cse803;
  uint64_t _cse802, _cse801, _cse800, _cse681, _cse680, _cse805, _cse804, _cse809, _cse808, _cse807, _cse806, _cse678;
  uint64_t _cse677, _cse811, _cse810, _cse815, _cse814, _cse813, _cse812, _cse675, _cse674, _cse817, _cse816, _cse822;
  uint64_t _cse821, _cse820, _cse819, _cse818, _cse672, _cse671, _cse670, _cse669, _cse824, _cse823, _cse825, _cse826;
  uint64_t _cse827, _cse828, _cse830, _cse831, _cse829, _cse832, _cse835, _cse834, _cse836, _cse833, _cse837, _cse838;
  uint64_t _cse839, _cse840, _cse843, _cse842, _cse841, _cse845, _cse846, _cse847, _cse844, _cse848, _cse851, _cse852;
  uint64_t _cse853, _cse850, _cse854, _cse849, _cse855, _cse856, _cse859, _cse858, _cse857, _cse861, _cse862, _cse863;
  uint64_t _cse860, _cse864, _cse867, _cse868, _cse869, _cse866, _cse870, _cse865, _cse871, _cse872, _cse873, _cse876;
  uint64_t _cse875, _cse874, _cse878, _cse879, _cse880, _cse877, _cse881, _cse884, _cse885, _cse886, _cse883, _cse887;
  uint64_t _cse882, _cse888, _cse889, _cse892, _cse891, _cse890, _cse894, _cse895, _cse896, _cse893, _cse897, _cse900;
  uint64_t _cse901, _cse902, _cse899, _cse903, _cse898, _cse904, _cse905, _cse906, _cse907, _cse908, _cse909, _cse910;
  uint64_t _cse911, _cse912, _cse913, _cse914, _cse915, _cse916, _cse917, _cse918, _cse920, _cse919, _cse922, _cse921;
  uint64_t _cse923, _cse924, _cse925, _cse926, _cse927, _cse928, _cse929, _cse930, _cse931, _cse932, _cse933, _cse934;
  uint64_t _cse935, _cse936, _cse937, _cse938, _cse939, _cse940, _cse941, _cse942, _cse943, _cse944, _cse945, _cse946;
  uint64_t _cse947, _cse948, _cse949, _cse950, _cse951, _cse952, _cse953, _cse954, _cse955, _cse956, _cse957, _cse958;
  uint64_t _cse959, _cse960, _cse961, _cse962, _cse963, _cse964, _cse965, _cse966, _cse967, _cse968, _cse969, _cse970;
  uint64_t _cse971, _cse972, _cse973, _cse974, _cse975, _cse976, _cse977, _cse978, _cse979, _cse980, _cse981, _cse982;
  uint64_t _cse983, _cse984, _cse985, _cse986, _cse987, _cse988, _cse989, _cse990, _cse991, _cse993, _cse994, _cse995;
  uint64_t _cse998, _cse997, _cse996, _cse1000, _cse1001, _cse1002, _cse999, _cse1003, _cse1004, _cse1005, _cse1006, _cse1007;
  uint64_t _cse1009, _cse1011, _cse1012, _cse1013, _cse1014, _cse1015, _cse1016, _cse1017, _cse1018, _cse1019, _cse1020, _cse1021;
  uint64_t _cse1022, _cse1023, _cse1024, _cse1025, _cse1026, _cse1027, _cse1028, _cse1029, _cse1030, _cse1031, _cse1032, _cse1033;
  uint64_t _cse1034, _cse1035, _cse1036, _cse1037, _cse1038, _cse1039, _cse1040, _cse1041, _cse1042, _cse1043, _cse1044, _cse1045;
  uint64_t _cse1046, _cse1047, _cse1048, _cse1050, _cse1049, _cse1051, _cse1052, _cse1053, _cse1054, _cse1055, _cse1056, _cse1057;
  uint64_t _cse1058, _cse1059, _cse1060, _cse1061, _cse1062, _cse1063, _cse1064, _cse1065, _cse1066, _cse1067, _cse1068, _cse1069;
  uint64_t _cse1070, _cse1071, _cse1072;
  uint32_t t8, t105, t106, t107, t108, t162, t163, t164, t165, t167, t168, t169;
  uint32_t t170, t171, t172, t173, t174, t175, t176, t177, t178, t179, t180, t181;
  uint32_t t182, t191, t192, t193, t194, t197, t198, t199, t200, t201, t202, t203;
  uint32_t t204, t268, t280, t281, t282, t283, t284, t285, t286, t287, t288, t289;
  uint32_t t290, t296, t297, t298, t306, t307, t308, t309, t310, t311, t312, t313;
  uint32_t t314, t397, t398, t399, t400, t401, t402, t403, t404, t408, t409, t410;
  uint32_t t413, t414, t415, t428, t429, t430, t431, t432, t433, t434, t435, t470;
  uint32_t t477, t478, t479, t480, t481, t482, t483, t484, t485, t497, t498, t499;
  uint32_t t500, t501, t502, t503, t504, t524, t585, t586, t587, t588, t647, t648;
  uint32_t t649, t650, t651, t652, t653, t654, t655, t656, t657, t658, t659, t660;
  uint32_t t661, t662, t672, t673, t674, t675, t676, t677, t678, t679, t680, t682;
  uint32_t t749, t750, t751, t752, t806, t807, t808, t809, t810, t811, t812, t813;
  uint32_t t814, t815, t816, t817, t818, t819, t820, t821, t826, t827, t828, t829;
  uint32_t t830, t831, t832, t833, t874, t876, t877, t878, t879, t880, t881, t882;
  uint32_t t883, t915, _cse0, _cse175, _cse332, _cse992, _cse1008, _cse1010;
  uint8_t t40, t41, t42, t43, t44, t45, t46, t47, t48, t49, t50, t51;
  uint8_t t52, t53, t54, t55, t56, t57, t58, t59, t60, t61, t62, t63;
  uint8_t t64, t65, t66, t67, t68, t69, t70, t71, t72, t73, t74, t75;
  uint8_t t76, t77, t78, t79, t80, t81, t82, t83, t84, t85, t86, t87;
  uint8_t t88, t89, t90, t91, t92, t93, t94, t95, t96, t97, t98, t99;
  uint8_t t100, t101, t102, t103, t271, t272, t273, t274, t276, t438, t439, t440;
  uint8_t t441, t442, t443, t444, t445, t446, t447, t448, t449, t450, t451, t452;
  uint8_t t453, t454, t455, t456, t457, t458, t459, t460, t461, t462, t463, t464;
  uint8_t t465, t466, t467, t468, t506, t507, t508, t509, t510, t511, t512, t513;
  uint8_t t514, t515, t516, t517, t518, t522, t529, t530, t531, t532, t533, t534;
  uint8_t t535, t536, t537, t538, t539, t540, t541, t542, t543, t544, t545, t546;
  uint8_t t547, t548, t549, t550, t551, t552, t553, t554, t555, t556, t557, t558;
  uint8_t t559, t560, t561, t562, t563, t564, t565, t566, t567, t568, t569, t570;
  uint8_t t571, t572, t573, t574, t575, t576, t577, t578, t579, t580, t581, t582;
  uint8_t t583, t584, t684, t685, t686, t687, t688, t689, t690, t691, t692, t693;
  uint8_t t694, t695, t696, t697, t698, t699, t700, t701, t702, t703, t704, t705;
  uint8_t t706, t707, t708, t709, t710, t711, t712, t713, t714, t715, t716, t717;
  uint8_t t718, t719, t720, t721, t722, t723, t724, t725, t726, t727, t728, t729;
  uint8_t t730, t731, t732, t733, t734, t735, t736, t737, t738, t739, t740, t741;
  uint8_t t742, t743, t744, t745, t746, t747, t835, t836, t837, t838, t839, t842;
  uint8_t t843, t844, t845, t846, t847, t848, t849, t850, t851, t852, t853, t854;
  uint8_t t855, t856, t857, t858, t859, t860, t861, t862, t863, t864, t865, t866;
  uint8_t t867, t868, t869, t870, t871, t872;

  // b0 @0x10004000
  var_1d08 = (uint64_t*)arg4;
  var_1d10 = (uint32_t*)arg3;
  var_1ad0 = arg2;
  var_1b30 = arg1;
  var_1b60 = 0xb0822ba6;
  var_1364 = 0xb0822ba6;
  var_1a88 = 0x3ba517f6;
  var_1a90 = 0x1a490432;
  var_1aa0 = 0xf43d0f4;
  var_1ab8 = 0x9422580;
  var_1ae8 = 0x5d17240;
  var_1b78 = 0xfffffff8;
  var_1b70 = 0xff0000;
  var_1bc0 = 0xff00;
  var_1d18 = 0xff00ff00;
  var_1b10 = 0x10;
  var_1b50 = 0xc3706155;
  var_1b20 = 0x848fe73;
  var_1b08 = 0xacc10972;
  var_1af0 = 0x9422581;
  var_1ae0 = 0x6ba3ad9b;
  var_1a98 = 0x0;
  var_1af8 = 0xbd82238;
  var_1aa8 = 0x40;
  var_1d30 = 0xffffffffcd7988c6;
  var_1d20 = 0xffffffffbd960277;
  var_1b58 = 0xe5654ed;
  var_1d00 = 0x4;
  var_1b48 = 0x47f5dbc8;
  var_1ac0 = 0x45fb9ded;
  var_1cf8 = 0x186a1;
  var_1ab0 = 0x177db652;
  var_1b00 = 0xf43d0f5;
  var_1b18 = 0xfffffff1;
  var_1ad8 = 0xfffffff0;
  var_1b28 = 0x46ffeb84;
  var_1d28 = 0xffffffffa87976bb;
  var_1b68 = 0xffffffffa4bdc6e9;
  var_1ac8 = 0xffffff80;
  var_1be8 = 0xfffff000;
  var_1b80 = 0xfffffffff6bae427;
  var_1b88 = 0x12132474;
  var_1b90 = 0xffffffff9436b72e;
  var_1b98 = 0xffffffff8b366d12;
  var_1ba0 = 0x69ab94c8;
  var_1ba8 = 0xffffffffcfdb953b;
  var_1bb0 = 0xffffffffe3c702cf;
  var_1bb8 = 0x48233422;
  var_1bc8 = 0xffffffff8f209322;
  var_1bf0 = 0xfffffe00;
  var_1bd0 = 0xffffffffaed47066;
  var_1bd8 = 0xffffffffc4d2faa2;
  var_1b40 = 0x2f1b9b0e;
  var_1b38 = 0x1a55a83a;
  var_1be0 = 0x25ac16a7;
  var_1bf8 = 0xffffffffe15e5e35;
  var_1c00 = 0xfffffffffbd6b4af;
  var_1c08 = 0xffffffffbc162b09;
  var_1c10 = 0x35f562aa;
  var_1c18 = 0x27ef30d3;
  var_1c20 = 0x5ea6b3b5;
  var_1c28 = 0x299cca2a;
  var_1c30 = 0xffffffffc56e9540;
  var_1c38 = 0x1f269216;
  var_1c40 = 0xffffffff98ca2340;
  var_1c48 = 0xffffffffd02ead4d;
  var_1c50 = 0x5bbad257;
  var_1c58 = 0xffffffffe6b95e33;
  var_1c60 = 0xffffffc0;
  var_1c68 = 0xffffffffedecac7a;
  var_1c70 = 0xfffffc00;
  var_1c78 = 0xffffffffedb098d7;
  var_1c80 = 0xffff8000;
  var_1c88 = 0x21149206;
  var_1c90 = 0xffe00000;
  var_1c98 = 0x4dd44b9a;
  var_1ca0 = 0x1d14b76d;
  var_1ca8 = 0xffffffffa971bdae;
  var_1cb0 = 0x41499ee4;
  var_1cb8 = 0xffffffffbecda772;
  var_1cc0 = 0x1a5dcda7;
  var_1cc8 = 0xffffffff89adf877;
  var_1cd0 = 0xffffffffbeab3f78;
  var_1cd8 = 0xffffffffc9579d4f;
  var_1ce0 = 0xffffffffc708d7b8;
  var_1ce8 = 0xffffffffa2dba837;
  var_1cf0 = 0x5cbd8717;
  t0 = arg4;
  t1 = 0x1f90;
  t2 = arg6;
  t3 = arg7;
  t4 = arg8;
  t5 = 0xffffffff8a19fa0b;
  t6 = 0xffffffff86b23cfc;
  t7 = (__entry_r125 - 0x1fa0);
L_0x10004d40:
  // b1 @0x10004d40
  t8 = var_1364;
  if ((((int64_t)(0x852eddc2) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) != 0x1)) {
    goto L_0x100051c0;
  }
  // b3 @0x10004d5c
  if ((((int64_t)(0xc2e9a26f) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
    // b33 @0x100064d0
    if ((((int64_t)(0xe0bbcd6e) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b35 @0x100064ec
      if ((((int64_t)(0xedd2a4cc) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
        // b119 @0x1000753c
        if ((((int64_t)(0xf4ab7144) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
          // b243 @0x10009780
          if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xf4ab7145) == 0x1)) {
            // b325 @0x10015564
            var_fc8 = (var_1ad0 & 0xffffffffffffffe0);
            var_1364 = 0x2f8e4f4d;
            var_1370 = var_1a98;
            var_fc0 = (var_1b30 + var_fc8);
            var_fb8 = ((uint64_t)(var_fd0) + var_fc8);
            var_fb0 = (var_1ad0 - var_fc8);
            goto L_0x10004d40;
          }
          goto L_0x1000979c;
        }
        // b121 @0x10007558
        if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xedd2a4cd) != 0x1)) {
          goto L_0x10007574;
        }
        // b293 @0x1000cbe0
        /* 1 argument slot(s) hold values this function never wrote: either the callee reads a stale register or a definition was lost here */
        t469 = vm_memset(((__entry_r125 - 0x1080) + (((int64_t)(int32_t)(var_c64)) & 0xffffffff)), 0x0, (((0x3e - ((int64_t)(int32_t)(var_4cc))) & 0xffffffff) + 0x1), 0x10016a2c, t1, t2, t3, t4);
        var_1364 = 0xe0bbcd6f;
        t0 = t1 = t2 = t3 = t4 = 0x0; /* t0, t1, t2, t3, t4: never set up on this edge */
        goto L_0x10004d40;
      }
      goto L_0x10006508;
    }
    // b79 @0x10006a3c
    if ((((int64_t)(0xcaa685fa) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) != 0x1)) {
      goto L_0x10007948;
    }
    // b81 @0x10006a58
    if ((((int64_t)(0xcf1cbe9c) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b215 @0x10008c08
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xcf1cbe9d) == 0x1)) {
        // b317 @0x10013368
        (*var_a00) = ((uint32_t)((((int64_t)(int32_t)(var_3f8)) + ((int64_t)(int32_t)(var_7c4)))));
        (*var_9f8) = ((uint32_t)((((int64_t)(int32_t)(var_3fc)) + ((int64_t)(int32_t)(var_7c0)))));
        (*var_9f0) = ((uint32_t)((((int64_t)(int32_t)(var_400)) + ((int64_t)(int32_t)(var_7bc)))));
        (*var_9e8) = ((uint32_t)((((int64_t)(int32_t)(var_7a4)) + ((int64_t)(int32_t)(var_7b8)))));
        (*var_9e0) = ((uint32_t)((((int64_t)(int32_t)(var_404)) + ((int64_t)(int32_t)(var_7b4)))));
        (*var_9d8) = ((uint32_t)((((int64_t)(int32_t)(var_3f4)) + ((int64_t)(int32_t)(var_7b0)))));
        (*var_9d0) = ((uint32_t)((((int64_t)(int32_t)(var_3f0)) + ((int64_t)(int32_t)(var_7ac)))));
        (*var_9c8) = ((uint32_t)((((int64_t)(int32_t)(var_7a0)) + ((int64_t)(int32_t)(var_7a8)))));
        (*var_a08) = ((*var_a08) + 0x200);
        t805 = var_1a98;
        _cse0 = ((uint32_t)(t805));
        (*var_a10) = _cse0;
        var_19f0 = _cse0;
        var_1364 = 0xcd7988c6;
        goto L_0x10004d40;
      }
      goto L_0x10008c24;
    }
    // b83 @0x10006a74
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xcaa685fb) != 0x1)) {
      goto L_0x10006a90;
    }
    // b285 @0x1000bf1c
    t411 = var_19f8;
    t412 = var_1aa8;
    _cse1 = (__entry_r125 - 0x1180);
    t413 = (*(uint32_t*)((_cse1 + (((t411 + var_1b18) & 0xffffffff) << 0x2))));
    t414 = (*(uint32_t*)((_cse1 + (((t411 + 0xfffffff9) & 0xffffffff) << 0x2))));
    t415 = (*(uint32_t*)((_cse1 + (((t411 + var_1ad8) & 0xffffffff) << 0x2))));
    _cse2 = (((int64_t)(int32_t)((*(uint32_t*)((_cse1 + (((t411 + 0xfffffffe) & 0xffffffff) << 0x2)))))) & 0xffffffff);
    _cse3 = (((int64_t)(int32_t)(t413)) & 0xffffffff);
    (*(uint32_t*)((_cse1 + (t411 << 0x2)))) = ((uint32_t)((((((int64_t)(int32_t)(t415)) + ((int64_t)(int32_t)(t414))) + ((((_cse2 >> 0x5) | (_cse2 << 0x1b)) ^ (_cse2 >> 0x2)) ^ ((_cse2 >> 0x9) | (_cse2 << 0x17)))) + ((((_cse3 >> 0xb) | (_cse3 << 0x15)) ^ (_cse3 >> 0x5)) ^ ((_cse3 >> 0x3) | (_cse3 << 0x1d))))));
    var_19f8 = (t411 + 0x1);
    if (((var_19f8 == t412) == 0x1)) {
      var_1364 = 0xffffffffc921fa00;
    } else {
      var_1364 = 0xffffffffcaa685fb;
    }
    if ((((t411 + 0x1) == t412) == 0x1)) {
      t0 = 0xffffffffc921fa00;
    } else {
      t0 = 0xffffffffcaa685fb;
    }
    _cse4 = ((int64_t)(int32_t)(t415));
    t1 = (_cse4 + ((int64_t)(int32_t)(t414)));
    _cse5 = (((int64_t)(int32_t)(t413)) & 0xffffffff);
    t2 = ((((_cse5 >> 0xb) | (_cse5 << 0x15)) ^ (_cse5 >> 0x5)) ^ ((_cse5 >> 0x3) | (_cse5 << 0x1d)));
    t3 = _cse4;
    t4 = (((t411 + 0xfffffff9) & 0xffffffff) << 0x2);
    goto L_0x10004d40;
L_0x100051c0:
    // b18 @0x100051c0
    if ((((int64_t)(var_1a88) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b46 @0x100066b4
      if ((((int64_t)(0x5b5b41f6) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) != 0x1)) {
        // b48 @0x100066d0
        if ((((int64_t)(0x46ffeb83) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
          // b128 @0x100075ec
          if ((((int64_t)(0x4ca4ff78) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) != 0x1)) {
            // b250 @0x10009858
            if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1b28) == 0x1)) {
              // b327 @0x100156a4
              t885 = var_fe0;
              (*(uint8_t*)((t885 + 0x5f))) = var_a21;
              t886 = var_fe0;
              (*(uint8_t*)((t885 + 0x5e))) = var_aa1;
              t887 = var_fe0;
              (*(uint8_t*)((t886 + 0x5d))) = var_b21;
              t888 = var_fe0;
              (*(uint8_t*)((t887 + 0x5c))) = var_ba1;
              t889 = var_fe0;
              (*(uint8_t*)((t888 + 0x5b))) = var_a31;
              t890 = var_fe0;
              (*(uint8_t*)((t889 + 0x5a))) = var_ab1;
              t891 = var_fe0;
              (*(uint8_t*)((t890 + 0x59))) = var_b31;
              t892 = var_fe0;
              (*(uint8_t*)((t891 + 0x58))) = var_bb1;
              t893 = var_fe0;
              (*(uint8_t*)((t892 + 0x57))) = var_a41;
              t894 = var_fe0;
              (*(uint8_t*)((t893 + 0x56))) = var_ac1;
              t895 = var_fe0;
              (*(uint8_t*)((t894 + 0x55))) = var_b41;
              t896 = var_fe0;
              (*(uint8_t*)((t895 + 0x54))) = var_bc1;
              t897 = var_fe0;
              (*(uint8_t*)((t896 + 0x53))) = var_a51;
              t898 = var_fe0;
              (*(uint8_t*)((t897 + 0x52))) = var_ad1;
              t899 = var_fe0;
              (*(uint8_t*)((t898 + 0x51))) = var_b51;
              t900 = var_fe0;
              (*(uint8_t*)((t899 + 0x50))) = var_bd1;
              t901 = var_fe0;
              (*(uint8_t*)((t900 + 0x4f))) = var_a61;
              t902 = var_fe0;
              (*(uint8_t*)((t901 + 0x4e))) = var_ae1;
              t903 = var_fe0;
              (*(uint8_t*)((t902 + 0x4d))) = var_b61;
              t904 = var_fe0;
              (*(uint8_t*)((t903 + 0x4c))) = var_be1;
              t905 = var_fe0;
              (*(uint8_t*)((t904 + 0x4b))) = var_a71;
              t906 = var_fe0;
              (*(uint8_t*)((t905 + 0x4a))) = var_af1;
              t907 = var_fe0;
              (*(uint8_t*)((t906 + 0x49))) = var_b71;
              t908 = var_fe0;
              (*(uint8_t*)((t907 + 0x48))) = var_bf1;
              t909 = var_fe0;
              (*(uint8_t*)((t908 + 0x47))) = var_a81;
              t910 = var_fe0;
              (*(uint8_t*)((t909 + 0x46))) = var_b01;
              t911 = var_fe0;
              (*(uint8_t*)((t910 + 0x45))) = var_b81;
              t912 = var_fe0;
              (*(uint8_t*)((t911 + 0x44))) = var_c01;
              t913 = var_fe0;
              (*(uint8_t*)((t912 + 0x43))) = var_a91;
              t914 = var_fe0;
              (*(uint8_t*)((t913 + 0x42))) = var_b11;
              (*(uint8_t*)((t914 + 0x41))) = var_b91;
              (*var_fd0) = var_c02;
              var_1364 = 0xb8d08280;
              goto L_0x10004d40;
            } else {
              goto L_0x10009870;
            }
          }
          // b130 @0x10007608
          if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x4ca4ff79) == 0x1)) {
            // b295 @0x1000cd34
            if (((var_500 < var_1aa8) == 0x1)) {
              var_1364 = 0xffffffffc2e9a270;
            } else {
              var_1364 = 0x1ff94f5f;
            }
            goto L_0x10004d40;
          }
          goto L_0x10007624;
        } else {
          goto L_0x100066ec;
        }
      }
      // b90 @0x10007028
      if ((((int64_t)(0x6845caee) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
        // b92 @0x10007044
        if ((((int64_t)(0x6ba3ad9a) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
          // b222 @0x10008d98
          if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1ae0) == 0x1)) {
            // b319 @0x10013a78
            t823 = var_1980;
            t824 = var_1aa8;
            t825 = var_1ae0;
            var_424 = var_199c;
            t826 = var_1998;
            var_420 = t826;
            t827 = var_1994;
            var_41c = t827;
            t828 = var_19a4;
            var_428 = t828;
            t829 = var_1988;
            var_418 = t829;
            t830 = var_1984;
            var_414 = t830;
            t831 = var_1990;
            _cse6 = (t823 << 0x2);
            t832 = (*(uint32_t*)(((__entry_r125 - 0x1180) + _cse6)));
            _cse7 = ((int64_t)(int32_t)(var_198c));
            t833 = (*(uint32_t*)(((((((_cse7 & 0xff) % 0x3) == var_1a98) == 0x1) ? 0x100002d0 : 0x100003d0) + _cse6)));
            var_c0c = ((uint32_t)(_cse8));
            var_c08 = ((uint32_t)(_cse11));
            _cse10 = ((int64_t)(int32_t)(var_199c));
            _cse9 = ((((((((int64_t)(int32_t)(t827)) & (~(_cse10))) | (_cse10 & ((int64_t)(int32_t)(t826)))) + ((int64_t)(int32_t)(t831))) + ((int64_t)(int32_t)(t832))) + (((((_cse10 & var_1ac8) >> 0x7) | (_cse10 << 0x19)) ^ (((_cse10 & 0xffffe000) >> 0xd) | (_cse10 << 0x13))) ^ (((_cse10 & 0xfffe0000) >> 0x11) | (_cse10 << 0xf)))) + ((int64_t)(int32_t)(t833)));
            _cse8 = (_cse9 + ((int64_t)(int32_t)(var_19a0)));
            _cse12 = ((int64_t)(int32_t)(t828));
            _cse13 = ((int64_t)(int32_t)(t829));
            _cse14 = ((int64_t)(int32_t)(t830));
            _cse11 = (((((_cse12 ^ _cse13) & _cse14) ^ (_cse12 & _cse13)) + (((((_cse14 & 0xffffffe0) >> 0x5) | (_cse14 << 0x1b)) ^ (((_cse14 & 0xfffff800) >> 0xb) | (_cse14 << 0x15))) ^ (((_cse14 & 0xfff80000) >> 0x13) | (_cse14 << 0xd)))) + _cse9);
            _cse15 = (t823 + 0x1);
            if (((_cse15 == t824) == 0x1)) {
              var_1364 = 0xffffffffe364fcc9;
            } else {
              var_1364 = t825;
            }
            var_1980 = _cse15;
            var_1984 = var_c08;
            var_1988 = t830;
            var_198c = ((uint32_t)(((_cse7 & 0xffffffff) + 0x1)));
            var_1990 = t827;
            var_1994 = t826;
            var_1998 = var_199c;
            var_199c = var_c0c;
            var_19a0 = t828;
            var_19a4 = t829;
            _cse16 = (t823 + 0x1);
            t0 = _cse16;
            _cse17 = ((int64_t)(int32_t)(t828));
            _cse18 = ((int64_t)(int32_t)(t829));
            _cse19 = (_cse17 & _cse18);
            t1 = (((_cse17 ^ _cse18) & ((int64_t)(int32_t)(t830))) ^ _cse19);
            t2 = _cse19;
            if (((_cse16 == t824) == 0x1)) {
              t3 = 0xffffffffe364fcc9;
            } else {
              t3 = t825;
            }
            t4 = _cse17;
            goto L_0x10004d40;
          }
          goto L_0x10008db0;
        }
        // b94 @0x10007060
        if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x6845caef) != 0x1)) {
          goto L_0x1000707c;
        }
        // b287 @0x1000c0c4
        t418 = var_fe0;
        t419 = var_1308;
        (*(uint64_t*)(t418)) = var_1310;
        (*(uint64_t*)((t418 + 0x8))) = t419;
        t420 = var_fe0;
        t421 = var_1300;
        (*(uint64_t*)((t418 + 0x18))) = var_12f8;
        (*(uint64_t*)((t418 + 0x10))) = t421;
        t422 = var_fe0;
        t423 = var_12f0;
        t424 = var_12e8;
        (*(uint64_t*)((t420 + 0x28))) = t424;
        (*(uint64_t*)((t420 + 0x20))) = t423;
        t425 = var_12e0;
        (*(uint64_t*)((t422 + 0x38))) = var_12d8;
        (*(uint64_t*)((t422 + 0x30))) = t425;
        var_1364 = 0x3ba517f7;
        t0 = t424;
        goto L_0x10004d40;
      }
      goto L_0x10007a14;
    }
    // b20 @0x100051d8
    if ((((int64_t)(var_1a90) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b22 @0x100051f0
      if ((((int64_t)(0x2eb81b38) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) != 0x1)) {
        goto L_0x1000520c;
      }
      // b110 @0x100071dc
      if ((((int64_t)(0x323eeb54) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
        // b236 @0x10009558
        if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x323eeb55) == 0x1)) {
          // b323 @0x100151c4
          t874 = var_4cc;
          var_c60 = (_cse20 & 0xffffffff);
          _cse20 = ((int64_t)(int32_t)(t874));
          (*(uint8_t*)(((__entry_r125 - 0x1080) + var_c60))) = 0xfc;
          var_c64 = ((uint32_t)((_cse20 + 0x1)));
          if (((var_c60 < 0x38) == 0x1)) {
            var_1364 = 0xffffffff89a592ff;
          } else {
            var_1364 = 0x566b9a51;
          }
          t0 = 0xffffffff89a592ff;
          goto L_0x10004d40;
        }
        goto L_0x10009574;
      }
      // b112 @0x100071f8
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x2eb81b39) != 0x1)) {
        goto L_0x10007214;
      }
      // b291 @0x1000c88c
      t428 = (*var_f50);
      var_c54 = t428;
      t429 = (*var_f48);
      var_c50 = t429;
      t430 = (*var_f40);
      var_c4c = t430;
      t431 = (*var_f38);
      var_c48 = t431;
      t432 = (*var_f30);
      var_c44 = t432;
      t433 = (*var_f28);
      var_c40 = t433;
      t434 = (*var_f20);
      var_c3c = t434;
      t435 = (*var_f18);
      var_c38 = t435;
      var_1364 = 0x3ce20c75;
      var_16c8 = var_1a98;
      var_16d4 = ((uint32_t)(var_1a98));
      var_16cc = t428;
      var_16d0 = t429;
      var_16e8 = t431;
      var_16e4 = t432;
      var_16e0 = t433;
      var_16dc = t434;
      var_16d8 = t435;
      var_16ec = t430;
      goto L_0x10004d40;
    }
    // b68 @0x10006994
    if ((((int64_t)(var_1aa0) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b70 @0x100069ac
      if ((((int64_t)(var_1ab0) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) != 0x1)) {
        // b72 @0x100069c4
        if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1b00) == 0x1)) {
          // b283 @0x1000bd74
          var_13c0 = var_f90;
          if (((var_f90 == var_1a98) == 0x1)) {
            var_1364 = 0xffffffff89fd6da1;
          } else {
            var_1364 = 0xffffffff839fc771;
          }
          var_13b0 = var_f80;
          var_13b8 = var_f78;
          goto L_0x10004d40;
        } else {
          goto L_0x100069dc;
        }
      }
      // b208 @0x10008360
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x177db653) == 0x1)) {
        // b315 @0x1001209c
        t684 = (*var_700);
        t685 = (*var_6f8);
        t686 = (*var_738);
        t687 = (*var_6f0);
        t688 = (*var_6e8);
        t689 = (*var_6e0);
        t690 = (*var_6d8);
        t691 = (*var_6d0);
        t692 = (*var_6c8);
        t693 = (*var_6c0);
        t694 = (*var_6b8);
        t695 = (*var_6b0);
        t696 = (*var_6a8);
        t697 = (*var_6a0);
        t698 = (*var_698);
        t699 = (*var_690);
        t700 = (*var_688);
        t701 = (*var_680);
        t702 = (*var_678);
        t703 = (*var_670);
        t704 = (*var_668);
        t705 = (*var_660);
        t706 = (*var_658);
        t707 = (*var_650);
        t708 = (*var_648);
        t709 = (*var_640);
        t710 = (*var_638);
        t711 = (*var_630);
        t712 = (*var_628);
        t713 = (*var_620);
        t714 = (*var_618);
        t715 = (*var_610);
        t716 = (*var_608);
        t717 = (*var_600);
        t718 = (*var_5f8);
        t719 = (*var_5f0);
        t720 = (*var_5e8);
        t721 = (*var_5e0);
        t722 = (*var_5d8);
        t723 = (*var_5d0);
        t724 = (*var_5c8);
        t725 = (*var_5c0);
        t726 = (*var_5b8);
        t727 = (*var_5b0);
        t728 = (*var_5a8);
        t729 = (*var_5a0);
        t730 = (*var_598);
        t731 = (*var_590);
        t732 = (*var_588);
        t733 = (*var_580);
        t734 = (*var_578);
        t735 = (*var_570);
        t736 = (*var_568);
        t737 = (*var_560);
        t738 = (*var_558);
        t739 = (*var_550);
        t740 = (*var_548);
        t741 = (*var_540);
        t742 = (*var_538);
        t743 = (*var_530);
        t744 = (*var_528);
        t745 = (*var_520);
        t746 = (*var_518);
        t747 = (*var_510);
        t748 = var_720;
        t749 = (*(uint32_t*)(t748));
        t750 = (*var_718);
        t751 = (*var_710);
        t752 = (*var_708);
        t753 = var_1b68;
        t754 = var_1ac8;
        t755 = var_1be8;
        t756 = var_1b80;
        t757 = var_1b88;
        t758 = var_1b90;
        t759 = var_1b98;
        t760 = var_1ba0;
        t761 = var_1ba8;
        t762 = var_1bb0;
        t763 = var_1bb8;
        t764 = var_1bc8;
        t765 = var_1bf0;
        t766 = var_1bd0;
        t767 = var_1bd8;
        t768 = var_1b40;
        t769 = var_1b38;
        t770 = var_1be0;
        t771 = var_1ad8;
        t772 = var_1bf8;
        t773 = var_1c00;
        t774 = var_1c08;
        t775 = var_1c10;
        t776 = var_1c18;
        t777 = var_1c20;
        t778 = var_1c28;
        t779 = var_1c30;
        t780 = var_1c38;
        t781 = var_1c40;
        t782 = var_1c48;
        t783 = var_1c50;
        t784 = var_1c58;
        t785 = var_1c60;
        t786 = var_1c68;
        t787 = var_1c70;
        t788 = var_1c78;
        t789 = var_1c80;
        t790 = var_1c88;
        t791 = var_1c90;
        t792 = var_1c98;
        t793 = var_1ca0;
        t794 = var_1ca8;
        t795 = var_1cb0;
        t796 = var_1cb8;
        t797 = var_1cc0;
        t798 = var_1cc8;
        t799 = var_1cd0;
        t800 = var_1cd8;
        t801 = var_1ce0;
        _cse23 = (((((((int64_t)(int8_t)(t729)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t728)) & 0xff)) | ((((int64_t)(int8_t)(t730)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t731)) << 0x18));
        _cse26 = (((((((int64_t)(int8_t)(t745)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t744)) & 0xff)) | ((((int64_t)(int8_t)(t746)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t747)) << 0x18));
        _cse29 = (((((((int64_t)(int8_t)(t697)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t696)) & 0xff)) | ((((int64_t)(int8_t)(t698)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t699)) << 0x18));
        _cse32 = (((((((int64_t)(int8_t)(t713)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t712)) & 0xff)) | ((((int64_t)(int8_t)(t714)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t715)) << 0x18));
        _cse35 = (((((((int64_t)(int8_t)(t733)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t732)) & 0xff)) | ((((int64_t)(int8_t)(t734)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t735)) << 0x18));
        _cse38 = (((((((int64_t)(int8_t)(t685)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t684)) & 0xff)) | ((((int64_t)(int8_t)(t686)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t687)) << 0x18));
        _cse41 = (((((((int64_t)(int8_t)(t701)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t700)) & 0xff)) | ((((int64_t)(int8_t)(t702)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t703)) << 0x18));
        _cse44 = (((((((int64_t)(int8_t)(t717)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t716)) & 0xff)) | ((((int64_t)(int8_t)(t718)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t719)) << 0x18));
        _cse47 = (((((((int64_t)(int8_t)(t693)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t692)) & 0xff)) | ((((int64_t)(int8_t)(t694)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t695)) << 0x18));
        _cse50 = (((((((int64_t)(int8_t)(t741)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t740)) & 0xff)) | ((((int64_t)(int8_t)(t742)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t743)) << 0x18));
        _cse53 = (((((((int64_t)(int8_t)(t725)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t724)) & 0xff)) | ((((int64_t)(int8_t)(t726)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t727)) << 0x18));
        _cse56 = (((((((int64_t)(int8_t)(t709)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t708)) & 0xff)) | ((((int64_t)(int8_t)(t710)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t711)) << 0x18));
        _cse59 = (((((((int64_t)(int8_t)(t737)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t736)) & 0xff)) | ((((int64_t)(int8_t)(t738)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t739)) << 0x18));
        _cse62 = (((((((int64_t)(int8_t)(t721)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t720)) & 0xff)) | ((((int64_t)(int8_t)(t722)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t723)) << 0x18));
        _cse65 = (((((((int64_t)(int8_t)(t705)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t704)) & 0xff)) | ((((int64_t)(int8_t)(t706)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t707)) << 0x18));
        _cse68 = (((((((int64_t)(int8_t)(t689)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t688)) & 0xff)) | ((((int64_t)(int8_t)(t690)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t691)) << 0x18));
        _cse69 = ((int64_t)(int32_t)(t752));
        _cse72 = ((int64_t)(int32_t)(t749));
        _cse74 = ((int64_t)(int32_t)(t751));
        _cse75 = ((int64_t)(int32_t)(t750));
        _cse73 = (_cse74 ^ _cse75);
        _cse71 = (((_cse38 + _cse72) + (_cse73 ^ _cse69)) + t753);
        _cse70 = ((((_cse71 & t754) >> 0x7) | (_cse71 << 0x19)) + _cse75);
        _cse67 = (((_cse68 + _cse69) + (_cse70 ^ _cse73)) + 0x554e80f9);
        _cse66 = ((((_cse67 & t755) >> 0xc) | (_cse67 << 0x14)) + _cse70);
        _cse83 = (_cse66 ^ _cse70);
        _cse82 = (((_cse47 + _cse74) + (_cse83 ^ _cse75)) + 0xffffffffd3d89f33);
        _cse81 = ((((_cse82 & 0xfffe0000) >> 0x11) | (_cse82 << 0xf)) + _cse66);
        _cse80 = (((_cse29 + _cse75) + (_cse81 ^ _cse83)) + 0x15a2af88);
        _cse79 = ((((_cse80 & 0xffc00000) >> 0x16) | (_cse80 << 0xa)) + _cse81);
        _cse78 = (_cse79 ^ _cse81);
        _cse77 = (((_cse41 + _cse70) + (_cse78 ^ _cse66)) + 0xffffffff924dac27);
        _cse76 = ((((_cse77 & t754) >> 0x7) | (_cse77 << 0x19)) + _cse79);
        _cse64 = (((_cse65 + _cse66) + (_cse76 ^ _cse78)) + 0x7e563339);
        _cse63 = ((((_cse64 & t755) >> 0xc) | (_cse64 << 0x14)) + _cse76);
        _cse89 = (_cse63 ^ _cse76);
        _cse88 = (((_cse56 + _cse81) + (_cse89 ^ _cse79)) + t756);
        _cse87 = ((((_cse88 & 0xfffe0000) >> 0x11) | (_cse88 << 0xf)) + _cse63);
        _cse86 = (((_cse32 + _cse79) + (_cse87 ^ _cse89)) + t757);
        _cse85 = ((((_cse86 & 0xffc00000) >> 0x16) | (_cse86 << 0xa)) + _cse87);
        _cse84 = (_cse85 ^ _cse87);
        _cse91 = (((_cse44 + _cse76) + (_cse84 ^ _cse63)) + t758);
        _cse90 = ((((_cse91 & t754) >> 0x7) | (_cse91 << 0x19)) + _cse85);
        _cse61 = (((_cse62 + _cse63) + (_cse84 ^ _cse90)) + t759);
        _cse60 = ((((_cse61 & t755) >> 0xc) | (_cse61 << 0x14)) + _cse90);
        _cse95 = (((_cse53 + _cse87) + ((_cse90 ^ _cse85) ^ _cse60)) + 0x23e22dcc);
        _cse94 = ((((_cse95 & 0xfffe0000) >> 0x11) | (_cse95 << 0xf)) + _cse60);
        _cse93 = (((_cse23 + _cse85) + ((_cse60 ^ _cse90) ^ _cse94)) + t760);
        _cse92 = ((((_cse93 & 0xffc00000) >> 0x16) | (_cse93 << 0xa)) + _cse94);
        _cse97 = (((_cse35 + _cse90) + ((_cse94 ^ _cse60) ^ _cse92)) + t761);
        _cse96 = ((((_cse97 & t754) >> 0x7) | (_cse97 << 0x19)) + _cse92);
        _cse58 = (((_cse59 + _cse60) + ((_cse92 ^ _cse94) ^ _cse96)) + t762);
        _cse57 = ((((_cse58 & t755) >> 0xc) | (_cse58 << 0x14)) + _cse96);
        _cse103 = (((_cse50 + _cse94) + ((_cse96 ^ _cse92) ^ _cse57)) + t763);
        _cse102 = ((((_cse103 & 0xfffe0000) >> 0x11) | (_cse103 << 0xf)) + _cse57);
        _cse101 = (((_cse26 + _cse92) + ((_cse57 ^ _cse96) ^ _cse102)) + 0xffffffffaad28857);
        _cse100 = ((((_cse101 & 0xffc00000) >> 0x16) | (_cse101 << 0xa)) + _cse102);
        _cse99 = (((_cse68 + _cse96) + ((_cse100 & _cse57) | (_cse102 & (~(_cse100))))) + 0xffffffff8de587b3);
        _cse98 = ((((_cse99 & 0xffffffe0) >> 0x5) | (_cse99 << 0x1b)) + _cse100);
        _cse55 = (((_cse56 + _cse57) + ((_cse98 & _cse102) | (_cse100 & (~(_cse98))))) + t764);
        _cse54 = ((((_cse55 & t765) >> 0x9) | (_cse55 << 0x17)) + _cse98);
        _cse109 = (((_cse23 + _cse102) + ((_cse54 & _cse100) | (_cse98 & (~(_cse54))))) + t766);
        _cse108 = ((((_cse109 & 0xffffc000) >> 0xe) | (_cse109 << 0x12)) + _cse54);
        _cse107 = (((_cse38 + _cse100) + ((_cse108 & _cse98) | (_cse54 & (~(_cse108))))) + 0x5e211c2c);
        _cse106 = ((((_cse107 & 0xfff00000) >> 0x14) | (_cse107 << 0xc)) + _cse108);
        _cse105 = (((_cse65 + _cse98) + ((_cse106 & _cse54) | (_cse108 & (~(_cse106))))) + 0xffffffffac5e94b2);
        _cse104 = ((((_cse105 & 0xffffffe0) >> 0x5) | (_cse105 << 0x1b)) + _cse106);
        _cse52 = (((_cse53 + _cse54) + ((_cse104 & _cse108) | (_cse106 & (~(_cse104))))) - 0x58b835e);
        _cse51 = ((((_cse52 & t765) >> 0x9) | (_cse52 << 0x17)) + _cse104);
        _cse115 = (((_cse26 + _cse108) + ((_cse51 & _cse106) | (_cse104 & (~(_cse51))))) + 0x674b87b8);
        _cse114 = ((((_cse115 & 0xffffc000) >> 0xe) | (_cse115 << 0x12)) + _cse51);
        _cse113 = (((_cse41 + _cse106) + ((_cse114 & _cse104) | (_cse51 & (~(_cse114))))) + t767);
        _cse112 = ((((_cse113 & 0xfff00000) >> 0x14) | (_cse113 << 0xc)) + _cse114);
        _cse111 = (((_cse62 + _cse104) + ((_cse112 & _cse51) | (_cse114 & (~(_cse112))))) + t768);
        _cse110 = ((((_cse111 & 0xffffffe0) >> 0x5) | (_cse111 << 0x1b)) + _cse112);
        _cse49 = (((_cse50 + _cse51) + ((_cse110 & _cse114) | (_cse112 & (~(_cse110))))) + t769);
        _cse48 = ((((_cse49 & t765) >> 0x9) | (_cse49 << 0x17)) + _cse110);
        _cse121 = (((_cse29 + _cse114) + ((_cse48 & _cse112) | (_cse110 & (~(_cse48))))) + 0xffffffff91bf8078);
        _cse120 = ((((_cse121 & 0xffffc000) >> 0xe) | (_cse121 << 0x12)) + _cse48);
        _cse119 = (((_cse44 + _cse112) + ((_cse120 & _cse110) | (_cse48 & (~(_cse120))))) + t6);
        _cse118 = ((((_cse119 & 0xfff00000) >> 0x14) | (_cse119 << 0xc)) + _cse120);
        _cse117 = (((_cse59 + _cse110) + ((_cse118 & _cse48) | (_cse120 & (~(_cse118))))) + 0x7c24dd24);
        _cse116 = ((((_cse117 & 0xffffffe0) >> 0x5) | (_cse117 << 0x1b)) + _cse118);
        _cse46 = (((_cse47 + _cse48) + ((_cse116 & _cse120) | (_cse118 & (~(_cse116))))) + t770);
        _cse45 = ((((_cse46 & t765) >> 0x9) | (_cse46 << 0x17)) + _cse116);
        _cse128 = (~(_cse45));
        _cse127 = (((_cse32 + _cse120) + ((_cse45 & _cse118) | (_cse116 & _cse128))) + 0xffffffffd88f1be9);
        _cse126 = ((((_cse127 & 0xffffc000) >> 0xe) | (_cse127 << 0x12)) + _cse45);
        _cse129 = (~(_cse126));
        _cse125 = (((_cse35 + _cse118) + ((_cse126 & _cse116) | (_cse45 & _cse129))) + t5);
        _cse124 = ((((_cse125 & 0xfff00000) >> 0x14) | (_cse125 << 0xc)) + _cse126);
        _cse123 = (((_cse65 + _cse116) + ((_cse124 & _cse128) | (_cse126 & _cse45))) + 0xffffffff813c91ac);
        _cse122 = ((((_cse123 & t771) >> 0x4) | (_cse123 << 0x1c)) + _cse124);
        _cse43 = (((_cse44 + _cse45) + ((_cse122 & _cse129) | (_cse124 & _cse126))) + 0xffffffffcf6eb59d);
        _cse42 = ((((_cse43 & 0xfffff800) >> 0xb) | (_cse43 << 0x15)) + _cse122);
        _cse135 = (((_cse23 + _cse126) + ((_cse42 & (~(_cse124))) | (_cse122 & _cse124))) + 0xffffffffafdbd5e2);
        _cse134 = ((((_cse135 & 0xffff0000) >> 0x10) | (_cse135 << 0x10)) + _cse42);
        _cse133 = (((_cse50 + _cse124) + ((_cse134 & (~(_cse122))) | (_cse42 & _cse122))) + 0xffffffff80e7c964);
        _cse132 = ((((_cse133 & 0xff800000) >> 0x17) | (_cse133 << 0x9)) + _cse134);
        _cse131 = (((_cse68 + _cse122) + ((_cse132 & (~(_cse42))) | (_cse134 & _cse42))) + t772);
        _cse130 = ((((_cse131 & t771) >> 0x4) | (_cse131 << 0x1c)) + _cse132);
        _cse40 = (((_cse41 + _cse42) + ((_cse130 & (~(_cse134))) | (_cse132 & _cse134))) + t773);
        _cse39 = ((((_cse40 & 0xfffff800) >> 0xb) | (_cse40 << 0x15)) + _cse130);
        _cse141 = (((_cse32 + _cse134) + ((_cse39 & (~(_cse132))) | (_cse130 & _cse132))) + t774);
        _cse140 = ((((_cse141 & 0xffff0000) >> 0x10) | (_cse141 << 0x10)) + _cse39);
        _cse139 = (((_cse53 + _cse132) + ((_cse140 & (~(_cse130))) | (_cse39 & _cse130))) + t775);
        _cse138 = ((((_cse139 & 0xff800000) >> 0x17) | (_cse139 << 0x9)) + _cse140);
        _cse137 = (((_cse59 + _cse130) + ((_cse138 & (~(_cse39))) | (_cse140 & _cse39))) + t776);
        _cse136 = ((((_cse137 & t771) >> 0x4) | (_cse137 << 0x1c)) + _cse138);
        _cse37 = (((_cse38 + _cse39) + ((_cse136 & (~(_cse140))) | (_cse138 & _cse140))) + t777);
        _cse36 = ((((_cse37 & 0xfffff800) >> 0xb) | (_cse37 << 0x15)) + _cse136);
        _cse147 = (((_cse29 + _cse140) + ((_cse36 & (~(_cse138))) | (_cse136 & _cse138))) + t778);
        _cse146 = ((((_cse147 & 0xffff0000) >> 0x10) | (_cse147 << 0x10)) + _cse36);
        _cse145 = (((_cse56 + _cse138) + ((_cse146 & (~(_cse136))) | (_cse36 & _cse136))) + t779);
        _cse144 = ((((_cse145 & 0xff800000) >> 0x17) | (_cse145 << 0x9)) + _cse146);
        _cse143 = (((_cse62 + _cse136) + ((_cse144 & (~(_cse36))) | (_cse146 & _cse36))) + t780);
        _cse142 = ((((_cse143 & t771) >> 0x4) | (_cse143 << 0x1c)) + _cse144);
        _cse34 = (((_cse35 + _cse36) + ((_cse142 & (~(_cse146))) | (_cse144 & _cse146))) + t781);
        _cse33 = ((((_cse34 & 0xfffff800) >> 0xb) | (_cse34 << 0x15)) + _cse142);
        _cse149 = (((_cse26 + _cse146) + ((_cse33 & (~(_cse144))) | (_cse142 & _cse144))) + t782);
        _cse148 = ((((_cse149 & 0xffff0000) >> 0x10) | (_cse149 << 0x10)) + _cse33);
        _cse153 = (((_cse47 + _cse144) + ((_cse148 & (~(_cse142))) | (_cse33 & _cse142))) + t783);
        _cse152 = ((((_cse153 & 0xff800000) >> 0x17) | (_cse153 << 0x9)) + _cse148);
        _cse151 = (((_cse38 + _cse142) + ((_cse33 | (~(_cse152))) ^ _cse148)) + t784);
        _cse150 = ((((_cse151 & t785) >> 0x6) | (_cse151 << 0x1a)) + _cse152);
        _cse31 = (((_cse32 + _cse33) + ((_cse148 | (~(_cse150))) ^ _cse152)) + t786);
        _cse30 = ((((_cse31 & t787) >> 0xa) | (_cse31 << 0x16)) + _cse150);
        _cse155 = (((_cse50 + _cse148) + ((_cse152 | (~(_cse30))) ^ _cse150)) + t788);
        _cse154 = ((((_cse155 & t789) >> 0xf) | (_cse155 << 0x11)) + _cse30);
        _cse159 = (((_cse65 + _cse152) + ((_cse150 | (~(_cse154))) ^ _cse30)) + t790);
        _cse158 = ((((_cse159 & t791) >> 0x15) | (_cse159 << 0xb)) + _cse154);
        _cse157 = (((_cse35 + _cse150) + ((_cse30 | (~(_cse158))) ^ _cse154)) + t792);
        _cse156 = ((((_cse157 & t785) >> 0x6) | (_cse157 << 0x1a)) + _cse158);
        _cse28 = (((_cse29 + _cse30) + ((_cse154 | (~(_cse156))) ^ _cse158)) + t793);
        _cse27 = ((((_cse28 & t787) >> 0xa) | (_cse28 << 0x16)) + _cse156);
        _cse161 = (((_cse53 + _cse154) + ((_cse158 | (~(_cse27))) ^ _cse156)) + t794);
        _cse160 = ((((_cse161 & t789) >> 0xf) | (_cse161 << 0x11)) + _cse27);
        _cse165 = (((_cse68 + _cse158) + ((_cse156 | (~(_cse160))) ^ _cse27)) + t795);
        _cse164 = ((((_cse165 & t791) >> 0x15) | (_cse165 << 0xb)) + _cse160);
        _cse163 = (((_cse44 + _cse156) + ((_cse27 | (~(_cse164))) ^ _cse160)) + t796);
        _cse162 = ((((_cse163 & t785) >> 0x6) | (_cse163 << 0x1a)) + _cse164);
        _cse25 = (((_cse26 + _cse27) + ((_cse160 | (~(_cse162))) ^ _cse164)) + t797);
        _cse24 = ((((_cse25 & t787) >> 0xa) | (_cse25 << 0x16)) + _cse162);
        _cse167 = (((_cse56 + _cse160) + ((_cse164 | (~(_cse24))) ^ _cse162)) + t798);
        _cse166 = ((((_cse167 & t789) >> 0xf) | (_cse167 << 0x11)) + _cse24);
        _cse171 = (((_cse59 + _cse164) + ((_cse162 | (~(_cse166))) ^ _cse24)) + t799);
        _cse170 = ((((_cse171 & t791) >> 0x15) | (_cse171 << 0xb)) + _cse166);
        _cse169 = (((_cse41 + _cse162) + ((_cse24 | (~(_cse170))) ^ _cse166)) + t800);
        _cse168 = ((((_cse169 & t785) >> 0x6) | (_cse169 << 0x1a)) + _cse170);
        _cse22 = (((_cse23 + _cse24) + ((_cse166 | (~(_cse168))) ^ _cse170)) + t801);
        _cse21 = ((((_cse22 & t787) >> 0xa) | (_cse22 << 0x16)) + _cse168);
        (*(uint32_t*)(t748)) = ((uint32_t)((_cse21 + _cse72)));
        t802 = var_1ce8;
        t803 = var_718;
        _cse173 = (((_cse47 + _cse166) + ((_cse170 | (~(_cse21))) ^ _cse168)) + t802);
        _cse172 = ((((_cse173 & t789) >> 0xf) | (_cse173 << 0x11)) + _cse21);
        _cse174 = (((_cse62 + _cse170) + ((_cse168 | (~(_cse172))) ^ _cse21)) + var_1cf0);
        (*(uint32_t*)(t803)) = ((uint32_t)(((_cse172 + _cse75) + (((_cse174 & t791) >> 0x15) | (_cse174 << 0xb)))));
        (*var_710) = ((uint32_t)((_cse172 + _cse74)));
        (*var_708) = ((uint32_t)((_cse168 + _cse69)));
        (*var_728) = ((*var_728) + 0x200);
        t804 = var_1a98;
        _cse175 = ((uint32_t)(t804));
        (*var_730) = _cse175;
        var_1a6c = _cse175;
        var_1364 = 0x180ab919;
        t0 = t803;
        _cse178 = (((((((int64_t)(int8_t)(t737)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t736)) & 0xff)) | ((((int64_t)(int8_t)(t738)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t739)) << 0x18));
        _cse181 = (((((((int64_t)(int8_t)(t689)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t688)) & 0xff)) | ((((int64_t)(int8_t)(t690)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t691)) << 0x18));
        _cse184 = (((((((int64_t)(int8_t)(t705)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t704)) & 0xff)) | ((((int64_t)(int8_t)(t706)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t707)) << 0x18));
        _cse187 = (((((((int64_t)(int8_t)(t693)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t692)) & 0xff)) | ((((int64_t)(int8_t)(t694)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t695)) << 0x18));
        _cse190 = (((((((int64_t)(int8_t)(t709)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t708)) & 0xff)) | ((((int64_t)(int8_t)(t710)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t711)) << 0x18));
        _cse193 = (((((((int64_t)(int8_t)(t725)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t724)) & 0xff)) | ((((int64_t)(int8_t)(t726)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t727)) << 0x18));
        _cse196 = (((((((int64_t)(int8_t)(t741)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t740)) & 0xff)) | ((((int64_t)(int8_t)(t742)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t743)) << 0x18));
        _cse199 = (((((((int64_t)(int8_t)(t733)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t732)) & 0xff)) | ((((int64_t)(int8_t)(t734)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t735)) << 0x18));
        _cse202 = (((((((int64_t)(int8_t)(t717)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t716)) & 0xff)) | ((((int64_t)(int8_t)(t718)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t719)) << 0x18));
        _cse205 = (((((((int64_t)(int8_t)(t701)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t700)) & 0xff)) | ((((int64_t)(int8_t)(t702)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t703)) << 0x18));
        _cse208 = (((((((int64_t)(int8_t)(t685)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t684)) & 0xff)) | ((((int64_t)(int8_t)(t686)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t687)) << 0x18));
        _cse211 = (((((((int64_t)(int8_t)(t745)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t744)) & 0xff)) | ((((int64_t)(int8_t)(t746)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t747)) << 0x18));
        _cse214 = (((((((int64_t)(int8_t)(t729)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t728)) & 0xff)) | ((((int64_t)(int8_t)(t730)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t731)) << 0x18));
        _cse217 = (((((((int64_t)(int8_t)(t713)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t712)) & 0xff)) | ((((int64_t)(int8_t)(t714)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t715)) << 0x18));
        _cse220 = (((((((int64_t)(int8_t)(t697)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t696)) & 0xff)) | ((((int64_t)(int8_t)(t698)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t699)) << 0x18));
        _cse221 = ((int64_t)(int32_t)(t750));
        _cse224 = ((int64_t)(int32_t)(t751));
        _cse228 = ((int64_t)(int32_t)(t752));
        _cse231 = ((int64_t)(int32_t)(t749));
        _cse232 = (_cse224 ^ _cse221);
        _cse230 = (((_cse208 + _cse231) + (_cse232 ^ _cse228)) + t753);
        _cse229 = ((((_cse230 & t754) >> 0x7) | (_cse230 << 0x19)) + _cse221);
        _cse227 = (((_cse181 + _cse228) + (_cse229 ^ _cse232)) + 0x554e80f9);
        _cse226 = ((((_cse227 & t755) >> 0xc) | (_cse227 << 0x14)) + _cse229);
        _cse225 = (_cse226 ^ _cse229);
        _cse223 = (((_cse187 + _cse224) + (_cse225 ^ _cse221)) + 0xffffffffd3d89f33);
        _cse222 = ((((_cse223 & 0xfffe0000) >> 0x11) | (_cse223 << 0xf)) + _cse226);
        _cse219 = (((_cse220 + _cse221) + (_cse222 ^ _cse225)) + 0x15a2af88);
        _cse218 = ((((_cse219 & 0xffc00000) >> 0x16) | (_cse219 << 0xa)) + _cse222);
        _cse240 = (_cse218 ^ _cse222);
        _cse239 = (((_cse205 + _cse229) + (_cse240 ^ _cse226)) + 0xffffffff924dac27);
        _cse238 = ((((_cse239 & t754) >> 0x7) | (_cse239 << 0x19)) + _cse218);
        _cse237 = (((_cse184 + _cse226) + (_cse238 ^ _cse240)) + 0x7e563339);
        _cse236 = ((((_cse237 & t755) >> 0xc) | (_cse237 << 0x14)) + _cse238);
        _cse235 = (_cse236 ^ _cse238);
        _cse234 = (((_cse190 + _cse222) + (_cse235 ^ _cse218)) + t756);
        _cse233 = ((((_cse234 & 0xfffe0000) >> 0x11) | (_cse234 << 0xf)) + _cse236);
        _cse216 = (((_cse217 + _cse218) + (_cse233 ^ _cse235)) + t757);
        _cse215 = ((((_cse216 & 0xffc00000) >> 0x16) | (_cse216 << 0xa)) + _cse233);
        _cse243 = (((((((int64_t)(int8_t)(t721)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t720)) & 0xff)) | ((((int64_t)(int8_t)(t722)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t723)) << 0x18));
        _cse244 = (_cse215 ^ _cse233);
        _cse246 = (((_cse202 + _cse238) + (_cse244 ^ _cse236)) + t758);
        _cse245 = ((((_cse246 & t754) >> 0x7) | (_cse246 << 0x19)) + _cse215);
        _cse242 = (((_cse243 + _cse236) + (_cse244 ^ _cse245)) + t759);
        _cse241 = ((((_cse242 & t755) >> 0xc) | (_cse242 << 0x14)) + _cse245);
        _cse248 = (((_cse193 + _cse233) + ((_cse245 ^ _cse215) ^ _cse241)) + 0x23e22dcc);
        _cse247 = ((((_cse248 & 0xfffe0000) >> 0x11) | (_cse248 << 0xf)) + _cse241);
        _cse213 = (((_cse214 + _cse215) + ((_cse241 ^ _cse245) ^ _cse247)) + t760);
        _cse212 = ((((_cse213 & 0xffc00000) >> 0x16) | (_cse213 << 0xa)) + _cse247);
        _cse252 = (((_cse199 + _cse245) + ((_cse247 ^ _cse241) ^ _cse212)) + t761);
        _cse251 = ((((_cse252 & t754) >> 0x7) | (_cse252 << 0x19)) + _cse212);
        _cse250 = (((_cse178 + _cse241) + ((_cse212 ^ _cse247) ^ _cse251)) + t762);
        _cse249 = ((((_cse250 & t755) >> 0xc) | (_cse250 << 0x14)) + _cse251);
        _cse254 = (((_cse196 + _cse247) + ((_cse251 ^ _cse212) ^ _cse249)) + t763);
        _cse253 = ((((_cse254 & 0xfffe0000) >> 0x11) | (_cse254 << 0xf)) + _cse249);
        _cse210 = (((_cse211 + _cse212) + ((_cse249 ^ _cse251) ^ _cse253)) + 0xffffffffaad28857);
        _cse209 = ((((_cse210 & 0xffc00000) >> 0x16) | (_cse210 << 0xa)) + _cse253);
        _cse260 = (((_cse181 + _cse251) + ((_cse209 & _cse249) | (_cse253 & (~(_cse209))))) + 0xffffffff8de587b3);
        _cse259 = ((((_cse260 & 0xffffffe0) >> 0x5) | (_cse260 << 0x1b)) + _cse209);
        _cse258 = (((_cse190 + _cse249) + ((_cse259 & _cse253) | (_cse209 & (~(_cse259))))) + t764);
        _cse257 = ((((_cse258 & t765) >> 0x9) | (_cse258 << 0x17)) + _cse259);
        _cse256 = (((_cse214 + _cse253) + ((_cse257 & _cse209) | (_cse259 & (~(_cse257))))) + t766);
        _cse255 = ((((_cse256 & 0xffffc000) >> 0xe) | (_cse256 << 0x12)) + _cse257);
        _cse207 = (((_cse208 + _cse209) + ((_cse255 & _cse259) | (_cse257 & (~(_cse255))))) + 0x5e211c2c);
        _cse206 = ((((_cse207 & 0xfff00000) >> 0x14) | (_cse207 << 0xc)) + _cse255);
        _cse266 = (((_cse184 + _cse259) + ((_cse206 & _cse257) | (_cse255 & (~(_cse206))))) + 0xffffffffac5e94b2);
        _cse265 = ((((_cse266 & 0xffffffe0) >> 0x5) | (_cse266 << 0x1b)) + _cse206);
        _cse264 = (((_cse193 + _cse257) + ((_cse265 & _cse255) | (_cse206 & (~(_cse265))))) - 0x58b835e);
        _cse263 = ((((_cse264 & t765) >> 0x9) | (_cse264 << 0x17)) + _cse265);
        _cse262 = (((_cse211 + _cse255) + ((_cse263 & _cse206) | (_cse265 & (~(_cse263))))) + 0x674b87b8);
        _cse261 = ((((_cse262 & 0xffffc000) >> 0xe) | (_cse262 << 0x12)) + _cse263);
        _cse204 = (((_cse205 + _cse206) + ((_cse261 & _cse265) | (_cse263 & (~(_cse261))))) + t767);
        _cse203 = ((((_cse204 & 0xfff00000) >> 0x14) | (_cse204 << 0xc)) + _cse261);
        _cse272 = (((_cse243 + _cse265) + ((_cse203 & _cse263) | (_cse261 & (~(_cse203))))) + t768);
        _cse271 = ((((_cse272 & 0xffffffe0) >> 0x5) | (_cse272 << 0x1b)) + _cse203);
        _cse270 = (((_cse196 + _cse263) + ((_cse271 & _cse261) | (_cse203 & (~(_cse271))))) + t769);
        _cse269 = ((((_cse270 & t765) >> 0x9) | (_cse270 << 0x17)) + _cse271);
        _cse268 = (((_cse220 + _cse261) + ((_cse269 & _cse203) | (_cse271 & (~(_cse269))))) + 0xffffffff91bf8078);
        _cse267 = ((((_cse268 & 0xffffc000) >> 0xe) | (_cse268 << 0x12)) + _cse269);
        _cse201 = (((_cse202 + _cse203) + ((_cse267 & _cse271) | (_cse269 & (~(_cse267))))) + t6);
        _cse200 = ((((_cse201 & 0xfff00000) >> 0x14) | (_cse201 << 0xc)) + _cse267);
        _cse278 = (((_cse178 + _cse271) + ((_cse200 & _cse269) | (_cse267 & (~(_cse200))))) + 0x7c24dd24);
        _cse277 = ((((_cse278 & 0xffffffe0) >> 0x5) | (_cse278 << 0x1b)) + _cse200);
        _cse276 = (((_cse187 + _cse269) + ((_cse277 & _cse267) | (_cse200 & (~(_cse277))))) + t770);
        _cse275 = ((((_cse276 & t765) >> 0x9) | (_cse276 << 0x17)) + _cse277);
        _cse279 = (~(_cse275));
        _cse274 = (((_cse217 + _cse267) + ((_cse275 & _cse200) | (_cse277 & _cse279))) + 0xffffffffd88f1be9);
        _cse273 = ((((_cse274 & 0xffffc000) >> 0xe) | (_cse274 << 0x12)) + _cse275);
        _cse280 = (~(_cse273));
        _cse198 = (((_cse199 + _cse200) + ((_cse273 & _cse277) | (_cse275 & _cse280))) + t5);
        _cse197 = ((((_cse198 & 0xfff00000) >> 0x14) | (_cse198 << 0xc)) + _cse273);
        _cse286 = (((_cse184 + _cse277) + ((_cse197 & _cse279) | (_cse273 & _cse275))) + 0xffffffff813c91ac);
        _cse285 = ((((_cse286 & t771) >> 0x4) | (_cse286 << 0x1c)) + _cse197);
        _cse284 = (((_cse202 + _cse275) + ((_cse285 & _cse280) | (_cse197 & _cse273))) + 0xffffffffcf6eb59d);
        _cse283 = ((((_cse284 & 0xfffff800) >> 0xb) | (_cse284 << 0x15)) + _cse285);
        _cse282 = (((_cse214 + _cse273) + ((_cse283 & (~(_cse197))) | (_cse285 & _cse197))) + 0xffffffffafdbd5e2);
        _cse281 = ((((_cse282 & 0xffff0000) >> 0x10) | (_cse282 << 0x10)) + _cse283);
        _cse195 = (((_cse196 + _cse197) + ((_cse281 & (~(_cse285))) | (_cse283 & _cse285))) + 0xffffffff80e7c964);
        _cse194 = ((((_cse195 & 0xff800000) >> 0x17) | (_cse195 << 0x9)) + _cse281);
        _cse292 = (((_cse181 + _cse285) + ((_cse194 & (~(_cse283))) | (_cse281 & _cse283))) + t772);
        _cse291 = ((((_cse292 & t771) >> 0x4) | (_cse292 << 0x1c)) + _cse194);
        _cse290 = (((_cse205 + _cse283) + ((_cse291 & (~(_cse281))) | (_cse194 & _cse281))) + t773);
        _cse289 = ((((_cse290 & 0xfffff800) >> 0xb) | (_cse290 << 0x15)) + _cse291);
        _cse288 = (((_cse217 + _cse281) + ((_cse289 & (~(_cse194))) | (_cse291 & _cse194))) + t774);
        _cse287 = ((((_cse288 & 0xffff0000) >> 0x10) | (_cse288 << 0x10)) + _cse289);
        _cse192 = (((_cse193 + _cse194) + ((_cse287 & (~(_cse291))) | (_cse289 & _cse291))) + t775);
        _cse191 = ((((_cse192 & 0xff800000) >> 0x17) | (_cse192 << 0x9)) + _cse287);
        _cse298 = (((_cse178 + _cse291) + ((_cse191 & (~(_cse289))) | (_cse287 & _cse289))) + t776);
        _cse297 = ((((_cse298 & t771) >> 0x4) | (_cse298 << 0x1c)) + _cse191);
        _cse296 = (((_cse208 + _cse289) + ((_cse297 & (~(_cse287))) | (_cse191 & _cse287))) + t777);
        _cse295 = ((((_cse296 & 0xfffff800) >> 0xb) | (_cse296 << 0x15)) + _cse297);
        _cse294 = (((_cse220 + _cse287) + ((_cse295 & (~(_cse191))) | (_cse297 & _cse191))) + t778);
        _cse293 = ((((_cse294 & 0xffff0000) >> 0x10) | (_cse294 << 0x10)) + _cse295);
        _cse189 = (((_cse190 + _cse191) + ((_cse293 & (~(_cse297))) | (_cse295 & _cse297))) + t779);
        _cse188 = ((((_cse189 & 0xff800000) >> 0x17) | (_cse189 << 0x9)) + _cse293);
        _cse304 = (((_cse243 + _cse297) + ((_cse188 & (~(_cse295))) | (_cse293 & _cse295))) + t780);
        _cse303 = ((((_cse304 & t771) >> 0x4) | (_cse304 << 0x1c)) + _cse188);
        _cse302 = (((_cse199 + _cse295) + ((_cse303 & (~(_cse293))) | (_cse188 & _cse293))) + t781);
        _cse301 = ((((_cse302 & 0xfffff800) >> 0xb) | (_cse302 << 0x15)) + _cse303);
        _cse300 = (((_cse211 + _cse293) + ((_cse301 & (~(_cse188))) | (_cse303 & _cse188))) + t782);
        _cse299 = ((((_cse300 & 0xffff0000) >> 0x10) | (_cse300 << 0x10)) + _cse301);
        _cse186 = (((_cse187 + _cse188) + ((_cse299 & (~(_cse303))) | (_cse301 & _cse303))) + t783);
        _cse185 = ((((_cse186 & 0xff800000) >> 0x17) | (_cse186 << 0x9)) + _cse299);
        _cse306 = (((_cse208 + _cse303) + ((_cse301 | (~(_cse185))) ^ _cse299)) + t784);
        _cse305 = ((((_cse306 & t785) >> 0x6) | (_cse306 << 0x1a)) + _cse185);
        _cse310 = (((_cse217 + _cse301) + ((_cse299 | (~(_cse305))) ^ _cse185)) + t786);
        _cse309 = ((((_cse310 & t787) >> 0xa) | (_cse310 << 0x16)) + _cse305);
        _cse308 = (((_cse196 + _cse299) + ((_cse185 | (~(_cse309))) ^ _cse305)) + t788);
        _cse307 = ((((_cse308 & t789) >> 0xf) | (_cse308 << 0x11)) + _cse309);
        _cse183 = (((_cse184 + _cse185) + ((_cse305 | (~(_cse307))) ^ _cse309)) + t790);
        _cse182 = ((((_cse183 & t791) >> 0x15) | (_cse183 << 0xb)) + _cse307);
        _cse312 = (((_cse199 + _cse305) + ((_cse309 | (~(_cse182))) ^ _cse307)) + t792);
        _cse311 = ((((_cse312 & t785) >> 0x6) | (_cse312 << 0x1a)) + _cse182);
        _cse316 = (((_cse220 + _cse309) + ((_cse307 | (~(_cse311))) ^ _cse182)) + t793);
        _cse315 = ((((_cse316 & t787) >> 0xa) | (_cse316 << 0x16)) + _cse311);
        _cse314 = (((_cse193 + _cse307) + ((_cse182 | (~(_cse315))) ^ _cse311)) + t794);
        _cse313 = ((((_cse314 & t789) >> 0xf) | (_cse314 << 0x11)) + _cse315);
        _cse180 = (((_cse181 + _cse182) + ((_cse311 | (~(_cse313))) ^ _cse315)) + t795);
        _cse179 = ((((_cse180 & t791) >> 0x15) | (_cse180 << 0xb)) + _cse313);
        _cse318 = (((_cse202 + _cse311) + ((_cse315 | (~(_cse179))) ^ _cse313)) + t796);
        _cse317 = ((((_cse318 & t785) >> 0x6) | (_cse318 << 0x1a)) + _cse179);
        _cse322 = (((_cse211 + _cse315) + ((_cse313 | (~(_cse317))) ^ _cse179)) + t797);
        _cse321 = ((((_cse322 & t787) >> 0xa) | (_cse322 << 0x16)) + _cse317);
        _cse320 = (((_cse190 + _cse313) + ((_cse179 | (~(_cse321))) ^ _cse317)) + t798);
        _cse319 = ((((_cse320 & t789) >> 0xf) | (_cse320 << 0x11)) + _cse321);
        _cse177 = (((_cse178 + _cse179) + ((_cse317 | (~(_cse319))) ^ _cse321)) + t799);
        _cse176 = ((((_cse177 & t791) >> 0x15) | (_cse177 << 0xb)) + _cse319);
        t1 = _cse176;
        _cse326 = (((_cse205 + _cse317) + ((_cse321 | (~(_cse176))) ^ _cse319)) + t800);
        _cse325 = ((((_cse326 & t785) >> 0x6) | (_cse326 << 0x1a)) + _cse176);
        _cse324 = (((_cse214 + _cse321) + ((_cse319 | (~(_cse325))) ^ _cse176)) + t801);
        _cse323 = ((((_cse324 & t787) >> 0xa) | (_cse324 << 0x16)) + _cse325);
        t2 = _cse323;
        t3 = (_cse323 + _cse231);
        t4 = t748;
        goto L_0x10004d40;
      }
      goto L_0x1000836c;
    }
    // b146 @0x10007884
    if ((((int64_t)(var_1ab8) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b148 @0x1000789c
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1af0) != 0x1)) {
        goto L_0x100078b4;
      }
      // b299 @0x1000e7a0
      t477 = (*var_f50);
      var_c2c = t477;
      t478 = (*var_f48);
      var_c28 = t478;
      t479 = (*var_f40);
      var_c24 = t479;
      t480 = (*var_f38);
      var_c20 = t480;
      t481 = (*var_f30);
      var_c1c = t481;
      t482 = (*var_f28);
      var_c18 = t482;
      t483 = (*var_f20);
      var_c14 = t483;
      t484 = (*var_f18);
      var_c10 = t484;
      var_1364 = ((uint32_t)(var_1ae0));
      var_1980 = var_1a98;
      var_198c = ((uint32_t)(var_1a98));
      var_1984 = t477;
      var_1988 = t478;
      var_19a0 = t480;
      var_199c = t481;
      var_1998 = t482;
      var_1994 = t483;
      var_1990 = t484;
      var_19a4 = t479;
      goto L_0x10004d40;
    }
    // b260 @0x100099f8
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1ae8) == 0x1)) {
      // b329 @0x10015a94
      t915 = (*var_a10);
      t916 = var_1b78;
      t917 = (*var_a08);
      _cse327 = (~((t917 + ((((int64_t)(int32_t)(t915)) << 0x3) & t916))));
      (*var_a08) = _cse327;
      (*var_7e0) = ((uint8_t)(_cse327));
      (*var_7d8) = ((uint8_t)((_cse327 >> 0x8)));
      _cse328 = (_cse327 >> 0x10);
      (*var_7d0) = ((uint8_t)(_cse328));
      (*var_7e8) = ((uint8_t)((_cse327 >> 0x18)));
      _cse329 = (_cse327 >> 0x20);
      (*var_800) = ((uint8_t)(_cse329));
      (*var_7f8) = ((uint8_t)((_cse327 >> 0x28)));
      _cse330 = (_cse327 >> 0x30);
      (*var_7f0) = ((uint8_t)(_cse330));
      _cse331 = (_cse327 >> 0x38);
      (*var_808) = ((uint8_t)(_cse331));
      (*var_4a8) = ((uint32_t)((((((((int64_t)(int8_t)((*var_9c0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_a18))) << 0x18)) | ((((int64_t)(int8_t)((*var_9b8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_9b0))) & 0xff))));
      (*var_4a0) = ((uint32_t)((((((((int64_t)(int8_t)((*var_9a0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_9a8))) << 0x18)) | ((((int64_t)(int8_t)((*var_998))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_990))) & 0xff))));
      (*var_498) = ((uint32_t)((((((((int64_t)(int8_t)((*var_980))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_988))) << 0x18)) | ((((int64_t)(int8_t)((*var_978))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_970))) & 0xff))));
      (*var_490) = ((uint32_t)((((((((int64_t)(int8_t)((*var_960))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_968))) << 0x18)) | ((((int64_t)(int8_t)((*var_958))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_950))) & 0xff))));
      (*var_488) = ((uint32_t)((((((((int64_t)(int8_t)((*var_940))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_948))) << 0x18)) | ((((int64_t)(int8_t)((*var_938))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_930))) & 0xff))));
      (*var_480) = ((uint32_t)((((((((int64_t)(int8_t)((*var_920))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_928))) << 0x18)) | ((((int64_t)(int8_t)((*var_918))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_910))) & 0xff))));
      (*var_478) = ((uint32_t)((((((((int64_t)(int8_t)((*var_900))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_908))) << 0x18)) | ((((int64_t)(int8_t)((*var_8f8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_8f0))) & 0xff))));
      (*var_470) = ((uint32_t)((((((((int64_t)(int8_t)((*var_8e0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_8e8))) << 0x18)) | ((((int64_t)(int8_t)((*var_8d8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_8d0))) & 0xff))));
      (*var_468) = ((uint32_t)((((((((int64_t)(int8_t)((*var_8c0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_8c8))) << 0x18)) | ((((int64_t)(int8_t)((*var_8b8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_8b0))) & 0xff))));
      (*var_460) = ((uint32_t)((((((((int64_t)(int8_t)((*var_8a0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_8a8))) << 0x18)) | ((((int64_t)(int8_t)((*var_898))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_890))) & 0xff))));
      (*var_458) = ((uint32_t)((((((((int64_t)(int8_t)((*var_880))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_888))) << 0x18)) | ((((int64_t)(int8_t)((*var_878))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_870))) & 0xff))));
      (*var_450) = ((uint32_t)((((((((int64_t)(int8_t)((*var_860))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_868))) << 0x18)) | ((((int64_t)(int8_t)((*var_858))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_850))) & 0xff))));
      (*var_448) = ((uint32_t)((((((((int64_t)(int8_t)((*var_840))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_848))) << 0x18)) | ((((int64_t)(int8_t)((*var_838))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_830))) & 0xff))));
      t918 = var_440;
      (*(uint32_t*)(t918)) = ((uint32_t)((((((((int64_t)(int8_t)((*var_820))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_828))) << 0x18)) | ((((int64_t)(int8_t)((*var_818))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_810))) & 0xff))));
      t919 = var_1b70;
      t920 = var_1bc0;
      t921 = var_438;
      (*(uint32_t*)(t921)) = ((uint32_t)(((((_cse330 & 0xff) | (_cse331 << 0x18)) | (_cse328 & t919)) | (_cse329 & t920))));
      (*var_430) = ((uint32_t)(((((_cse327 << 0x10) & t919) | (_cse327 & var_1d18)) | (_cse328 & 0xff))));
      var_1a30 = var_1b10;
      var_1364 = ((uint32_t)(var_1b50));
      t0 = t921;
      t1 = t920;
      t2 = t919;
      t3 = t918;
      goto L_0x10004d40;
    }
    // b262 @0x10009a10
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1b20) == 0x1)) {
      // b264 @0x10009a28
      var_1364 = ((uint32_t)(var_1b08));
      var_1390 = var_1378;
      var_1398 = var_1380;
      var_13a0 = var_1388;
    }
    goto L_0x10004d40;
L_0x100078b4:
    // b150 @0x100078b4
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1af8) == 0x1)) {
      // b300 @0x1000e980
      var_410 = var_19b0;
      (*(uint8_t*)(((__entry_r125 - 0x11f0) + (((int64_t)(int32_t)(var_19b4)) & 0xffffffff)))) = (*(uint8_t*)((var_fe0 + var_19b0)));
      t485 = (*var_a10);
      t486 = var_1aa8;
      t487 = var_1d30;
      t488 = var_1d20;
      _cse333 = (((int64_t)(int32_t)(t485)) + 0x1);
      _cse332 = ((uint32_t)(_cse333));
      (*var_a10) = _cse332;
      var_19f0 = _cse332;
      if ((((_cse333 & 0xffffffff) == t486) == 0x1)) {
        var_1364 = t488;
      } else {
        var_1364 = t487;
      }
      t0 = t488;
      goto L_0x10004d40;
    }
    // b152 @0x100078cc
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1b58) == 0x1)) {
      // b154 @0x100078e4
      t183 = (*var_1d08);
      var_1a7c = ((uint32_t)(var_1cf8));
      if (((t183 < var_1d00) == 0x1)) {
        var_1364 = var_1ac0;
      } else {
        var_1364 = var_1b48;
      }
    }
    goto L_0x10004d40;
L_0x100069dc:
    // b74 @0x100069dc
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x1557b4b0) == 0x1)) {
      // b284 @0x1000bdf0
      t405 = var_1978;
      t406 = var_1aa8;
      t407 = var_1af0;
      _cse334 = (__entry_r125 - 0x1180);
      t408 = (*(uint32_t*)((_cse334 + (((t405 + var_1b18) & 0xffffffff) << 0x2))));
      t409 = (*(uint32_t*)((_cse334 + (((t405 + 0xfffffff9) & 0xffffffff) << 0x2))));
      t410 = (*(uint32_t*)((_cse334 + (((t405 + var_1ad8) & 0xffffffff) << 0x2))));
      _cse335 = (((int64_t)(int32_t)((*(uint32_t*)((_cse334 + (((t405 + 0xfffffffe) & 0xffffffff) << 0x2)))))) & 0xffffffff);
      _cse336 = (((int64_t)(int32_t)(t408)) & 0xffffffff);
      (*(uint32_t*)((_cse334 + (t405 << 0x2)))) = ((uint32_t)((((((int64_t)(int32_t)(t410)) + ((int64_t)(int32_t)(t409))) + ((((_cse335 >> 0x5) | (_cse335 << 0x1b)) ^ (_cse335 >> 0x2)) ^ ((_cse335 >> 0x9) | (_cse335 << 0x17)))) + ((((_cse336 >> 0xb) | (_cse336 << 0x15)) ^ (_cse336 >> 0x5)) ^ ((_cse336 >> 0x3) | (_cse336 << 0x1d))))));
      var_1978 = (t405 + 0x1);
      if (((var_1978 == t406) == 0x1)) {
        var_1364 = t407;
      } else {
        var_1364 = 0x1557b4b0;
      }
      if ((((t405 + 0x1) == t406) == 0x1)) {
        t0 = t407;
      } else {
        t0 = 0x1557b4b0;
      }
      _cse337 = ((int64_t)(int32_t)(t410));
      t1 = (_cse337 + ((int64_t)(int32_t)(t409)));
      _cse338 = (((int64_t)(int32_t)(t408)) & 0xffffffff);
      t2 = ((((_cse338 >> 0xb) | (_cse338 << 0x15)) ^ (_cse338 >> 0x5)) ^ ((_cse338 >> 0x3) | (_cse338 << 0x1d)));
      t3 = _cse337;
      t4 = (((t405 + 0xfffffff9) & 0xffffffff) << 0x2);
      goto L_0x10004d40;
    }
    // b76 @0x100069e8
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x169b2590) == 0x1)) {
      // b78 @0x100069f4
      if ((((uint64_t)(var_fd0) < (uint64_t)(var_fd8)) == 0x1)) {
        var_1364 = var_1d28;
      } else {
        var_1364 = var_1b28;
      }
    }
    goto L_0x10004d40;
L_0x1000836c:
    // b210 @0x1000836c
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x180ab919) == 0x1)) {
      // b316 @0x100132f8
      var_3ac = var_1a6c;
      var_1a68 = (var_3b8 + 0x1);
      var_1a60 = var_1a6c;
      if (((var_1a68 == 0x20) == 0x1)) {
        var_1364 = 0x1a490433;
      } else {
        var_1364 = 0xffffffffa259f5ae;
      }
      t0 = 0x1a490433;
      goto L_0x10004d40;
    }
    // b212 @0x10008378
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x19b80fb0) == 0x1)) {
      // b214 @0x10008394
      var_4a8 = (uint32_t*)var_1910;
      t205 = var_18f0;
      t206 = var_18f8;
      t207 = var_1900;
      t208 = var_1908;
      t209 = var_1918;
      t210 = var_1920;
      t211 = var_1928;
      t212 = var_1938;
      t213 = var_1940;
      t214 = var_1948;
      t215 = var_1950;
      t216 = var_1958;
      t217 = var_1960;
      t218 = var_1968;
      t219 = var_1970;
      var_4a0 = (uint32_t*)var_18e8;
      var_498 = (uint32_t*)var_18c0;
      var_490 = (uint32_t*)var_1898;
      t220 = var_1878;
      t221 = var_1880;
      t222 = var_1888;
      t223 = var_1890;
      t224 = var_18a0;
      t225 = var_18a8;
      t226 = var_18b0;
      t227 = var_18b8;
      t228 = var_18c8;
      t229 = var_18d0;
      t230 = var_18d8;
      t231 = var_18e0;
      var_488 = (uint32_t*)var_1870;
      var_480 = (uint32_t*)var_1848;
      var_478 = (uint32_t*)var_1820;
      t232 = var_1800;
      t233 = var_1808;
      t234 = var_1810;
      t235 = var_1818;
      t236 = var_1828;
      t237 = var_1830;
      t238 = var_1838;
      t239 = var_1840;
      t240 = var_1850;
      t241 = var_1858;
      t242 = var_1860;
      t243 = var_1868;
      var_470 = (uint32_t*)var_17f8;
      t244 = var_17d8;
      t245 = var_17e0;
      t246 = var_17e8;
      t247 = var_17f0;
      var_468 = (uint32_t*)var_17d0;
      t248 = var_17b0;
      t249 = var_17b8;
      t250 = var_17c0;
      t251 = var_17c8;
      var_460 = (uint32_t*)var_17a8;
      t252 = var_1788;
      t253 = var_1790;
      t254 = var_1798;
      t255 = var_17a0;
      var_458 = (uint32_t*)var_1780;
      t256 = var_1760;
      t257 = var_1768;
      t258 = var_1770;
      t259 = var_1778;
      var_450 = (uint32_t*)var_1758;
      t260 = var_1738;
      t261 = var_1740;
      t262 = var_1748;
      t263 = var_1750;
      var_448 = (uint32_t*)var_1730;
      t264 = var_1710;
      t265 = var_1718;
      t266 = var_1720;
      t267 = var_1728;
      var_440 = (uint32_t*)var_1708;
      var_438 = (uint32_t*)var_1700;
      var_430 = (uint32_t*)var_16f8;
      t268 = (*var_f60);
      t269 = var_1b78;
      t270 = (*var_f58);
      _cse339 = (~((t270 + ((((int64_t)(int32_t)(t268)) << 0x3) & t269))));
      (*var_f58) = _cse339;
      (*(uint8_t*)(t219)) = ((uint8_t)(_cse339));
      (*(uint8_t*)(t218)) = ((uint8_t)((_cse339 >> 0x8)));
      _cse340 = (_cse339 >> 0x10);
      (*(uint8_t*)(t217)) = ((uint8_t)(_cse340));
      (*(uint8_t*)(t216)) = ((uint8_t)((_cse339 >> 0x18)));
      _cse341 = (_cse339 >> 0x20);
      (*(uint8_t*)(t215)) = ((uint8_t)(_cse341));
      (*(uint8_t*)(t214)) = ((uint8_t)((_cse339 >> 0x28)));
      _cse342 = (_cse339 >> 0x30);
      (*(uint8_t*)(t213)) = ((uint8_t)(_cse342));
      _cse343 = (_cse339 >> 0x38);
      (*(uint8_t*)(t212)) = ((uint8_t)(_cse343));
      t271 = (*var_f68);
      t272 = (*(uint8_t*)(t211));
      t273 = (*(uint8_t*)(t210));
      t274 = (*(uint8_t*)(t209));
      t275 = var_4a8;
      (*(uint32_t*)(t275)) = ((uint32_t)((((((((int64_t)(int8_t)(t272)) & 0xff) << 0x10) | (((int64_t)(int8_t)(t271)) << 0x18)) | ((((int64_t)(int8_t)(t273)) & 0xff) << 0x8)) | (((int64_t)(int8_t)(t274)) & 0xff))));
      t276 = (*(uint8_t*)(t208));
      (*var_4a0) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t207)))) & 0xff) << 0x10) | (((int64_t)(int8_t)(t276)) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t206)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t205)))) & 0xff))));
      (*var_498) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t230)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t231)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t229)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t228)))) & 0xff))));
      (*var_490) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t226)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t227)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t225)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t224)))) & 0xff))));
      (*var_488) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t222)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t223)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t221)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t220)))) & 0xff))));
      (*var_480) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t242)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t243)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t241)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t240)))) & 0xff))));
      (*var_478) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t238)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t239)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t237)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t236)))) & 0xff))));
      (*var_470) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t234)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t235)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t233)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t232)))) & 0xff))));
      (*var_468) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t246)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t247)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t245)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t244)))) & 0xff))));
      (*var_460) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t250)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t251)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t249)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t248)))) & 0xff))));
      (*var_458) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t254)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t255)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t253)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t252)))) & 0xff))));
      (*var_450) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t258)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t259)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t257)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t256)))) & 0xff))));
      (*var_448) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t262)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t263)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t261)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t260)))) & 0xff))));
      (*var_440) = ((uint32_t)((((((((int64_t)(int8_t)((*(uint8_t*)(t266)))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*(uint8_t*)(t267)))) << 0x18)) | ((((int64_t)(int8_t)((*(uint8_t*)(t265)))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*(uint8_t*)(t264)))) & 0xff))));
      t277 = var_1b70;
      (*var_438) = ((uint32_t)(((((_cse342 & 0xff) | (_cse343 << 0x18)) | (_cse340 & t277)) | (_cse341 & var_1bc0))));
      (*var_430) = ((uint32_t)(((((_cse339 << 0x10) & t277) | (_cse339 & var_1d18)) | (_cse340 & 0xff))));
      var_1978 = var_1b10;
      var_1364 = 0x1557b4b0;
      t0 = (((int64_t)(int8_t)(t276)) << 0x18);
      _cse345 = ((((int64_t)(int8_t)(t272)) & 0xff) << 0x10);
      _cse347 = ((int64_t)(int8_t)(t271));
      _cse346 = (_cse347 << 0x18);
      _cse344 = (_cse345 | _cse346);
      t1 = ((_cse344 | ((((int64_t)(int8_t)(t273)) & 0xff) << 0x8)) | (((int64_t)(int8_t)(t274)) & 0xff));
      t2 = t275;
      t3 = _cse344;
      t4 = _cse346;
    }
    goto L_0x10004d40;
L_0x1000520c:
    // b24 @0x1000520c
    if ((((int64_t)(0x263afc18) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b180 @0x10007bb0
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x263afc19) != 0x1)) {
        goto L_0x10007bcc;
      }
      // b307 @0x1000fd40
      t524 = (*var_730);
      t525 = var_1b78;
      t526 = (*var_728);
      _cse349 = (t526 + ((((int64_t)(int32_t)(t524)) << 0x3) & t525));
      _cse348 = (_cse349 ^ 0x64130efb);
      (*var_728) = _cse348;
      (*var_538) = ((uint8_t)(_cse348));
      (*var_540) = ((uint8_t)((_cse348 >> 0x8)));
      _cse350 = (_cse348 >> 0x10);
      (*var_548) = ((uint8_t)(_cse350));
      (*var_530) = ((uint8_t)((_cse348 >> 0x18)));
      _cse351 = (_cse349 >> 0x20);
      (*var_518) = ((uint8_t)(_cse351));
      (*var_520) = ((uint8_t)((_cse349 >> 0x28)));
      _cse352 = (_cse349 >> 0x30);
      (*var_528) = ((uint8_t)(_cse352));
      _cse353 = (_cse349 >> 0x38);
      (*var_510) = ((uint8_t)(_cse353));
      t527 = var_1bc0;
      t528 = var_1b70;
      t529 = (*var_700);
      t530 = (*var_6f8);
      t531 = (*var_738);
      t532 = (*var_6f0);
      t533 = (*var_6e8);
      t534 = (*var_6e0);
      t535 = (*var_6d8);
      t536 = (*var_6d0);
      t537 = (*var_6c8);
      t538 = (*var_6c0);
      t539 = (*var_6b8);
      t540 = (*var_6b0);
      t541 = (*var_6a8);
      t542 = (*var_6a0);
      t543 = (*var_698);
      t544 = (*var_690);
      t545 = (*var_688);
      t546 = (*var_680);
      t547 = (*var_678);
      t548 = (*var_670);
      t549 = (*var_668);
      t550 = (*var_660);
      t551 = (*var_658);
      t552 = (*var_650);
      t553 = (*var_648);
      t554 = (*var_640);
      t555 = (*var_638);
      t556 = (*var_630);
      t557 = (*var_628);
      t558 = (*var_620);
      t559 = (*var_618);
      t560 = (*var_610);
      t561 = (*var_608);
      t562 = (*var_600);
      t563 = (*var_5f8);
      t564 = (*var_5f0);
      t565 = (*var_5e8);
      t566 = (*var_5e0);
      t567 = (*var_5d8);
      t568 = (*var_5d0);
      t569 = (*var_5c8);
      t570 = (*var_5c0);
      t571 = (*var_5b8);
      t572 = (*var_5b0);
      t573 = (*var_5a8);
      t574 = (*var_5a0);
      t575 = (*var_598);
      t576 = (*var_590);
      t577 = (*var_588);
      t578 = (*var_580);
      t579 = (*var_578);
      t580 = (*var_570);
      t581 = (*var_568);
      t582 = (*var_560);
      t583 = (*var_558);
      t584 = (*var_550);
      t585 = (*var_720);
      t586 = (*var_718);
      t587 = (*var_710);
      t588 = (*var_708);
      t589 = var_1b68;
      t590 = var_1ac8;
      t591 = var_1be8;
      t592 = var_1b80;
      t593 = var_1b88;
      t594 = var_1b90;
      t595 = var_1b98;
      t596 = var_1ba0;
      t597 = var_1ba8;
      t598 = var_1bb0;
      t599 = var_1bb8;
      t600 = var_1bc8;
      t601 = var_1bf0;
      t602 = var_1bd0;
      t603 = var_1bd8;
      t604 = var_1b40;
      t605 = var_1b38;
      t606 = var_1be0;
      t607 = var_1ad8;
      t608 = var_1bf8;
      t609 = var_1c00;
      t610 = var_1c08;
      t611 = var_1c10;
      t612 = var_1c18;
      t613 = var_1c20;
      t614 = var_1c28;
      t615 = var_1c30;
      t616 = var_1c38;
      t617 = var_1c40;
      t618 = var_1c48;
      t619 = var_1c50;
      t620 = var_1c58;
      t621 = var_1c60;
      t622 = var_1c68;
      t623 = var_1c70;
      t624 = var_1c78;
      t625 = var_1c80;
      t626 = var_1c88;
      t627 = var_1c90;
      t628 = var_1c98;
      t629 = var_1ca0;
      t630 = var_1ca8;
      t631 = var_1cb0;
      t632 = var_1cb8;
      t633 = var_1cc0;
      t634 = var_1cc8;
      t635 = var_1cd0;
      t636 = var_1cd8;
      t637 = var_1ce0;
      var_4f3 = ((uint8_t)(_cse354));
      _cse357 = (((((((int64_t)(int8_t)(t574)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t573)) & 0xff)) | ((((int64_t)(int8_t)(t575)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t576)) << 0x18));
      _cse360 = ((((_cse352 & 0xff) | (_cse353 << 0x18)) | (_cse351 & t527)) | ((_cse349 >> 0x10) & t528));
      _cse363 = (((((((int64_t)(int8_t)(t542)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t541)) & 0xff)) | ((((int64_t)(int8_t)(t543)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t544)) << 0x18));
      _cse366 = (((((((int64_t)(int8_t)(t558)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t557)) & 0xff)) | ((((int64_t)(int8_t)(t559)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t560)) << 0x18));
      _cse369 = (((((((int64_t)(int8_t)(t578)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t577)) & 0xff)) | ((((int64_t)(int8_t)(t579)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t580)) << 0x18));
      _cse372 = (((((((int64_t)(int8_t)(t530)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t529)) & 0xff)) | ((((int64_t)(int8_t)(t531)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t532)) << 0x18));
      _cse375 = (((((((int64_t)(int8_t)(t546)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t545)) & 0xff)) | ((((int64_t)(int8_t)(t547)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t548)) << 0x18));
      _cse378 = (((((((int64_t)(int8_t)(t562)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t561)) & 0xff)) | ((((int64_t)(int8_t)(t563)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t564)) << 0x18));
      _cse381 = (((((((int64_t)(int8_t)(t538)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t537)) & 0xff)) | ((((int64_t)(int8_t)(t539)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t540)) << 0x18));
      _cse384 = (((_cse350 & 0xff) | (_cse348 & 0xffffffffff00ff00)) | ((_cse348 << 0x10) & t528));
      _cse387 = (((((((int64_t)(int8_t)(t570)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t569)) & 0xff)) | ((((int64_t)(int8_t)(t571)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t572)) << 0x18));
      _cse390 = (((((((int64_t)(int8_t)(t554)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t553)) & 0xff)) | ((((int64_t)(int8_t)(t555)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t556)) << 0x18));
      _cse393 = (((((((int64_t)(int8_t)(t582)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t581)) & 0xff)) | ((((int64_t)(int8_t)(t583)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t584)) << 0x18));
      _cse396 = (((((((int64_t)(int8_t)(t566)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t565)) & 0xff)) | ((((int64_t)(int8_t)(t567)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t568)) << 0x18));
      _cse399 = (((((((int64_t)(int8_t)(t550)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t549)) & 0xff)) | ((((int64_t)(int8_t)(t551)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t552)) << 0x18));
      _cse402 = (((((((int64_t)(int8_t)(t534)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t533)) & 0xff)) | ((((int64_t)(int8_t)(t535)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t536)) << 0x18));
      _cse403 = ((int64_t)(int32_t)(t588));
      _cse406 = ((int64_t)(int32_t)(t585));
      _cse408 = ((int64_t)(int32_t)(t587));
      _cse409 = ((int64_t)(int32_t)(t586));
      _cse407 = (_cse408 ^ _cse409);
      _cse405 = (((_cse372 + _cse406) + (_cse407 ^ _cse403)) + t589);
      _cse404 = ((((_cse405 & t590) >> 0x7) | (_cse405 << 0x19)) + _cse409);
      _cse401 = (((_cse402 + _cse403) + (_cse404 ^ _cse407)) + 0x554e80f9);
      _cse400 = ((((_cse401 & t591) >> 0xc) | (_cse401 << 0x14)) + _cse404);
      _cse417 = (_cse400 ^ _cse404);
      _cse416 = (((_cse381 + _cse408) + (_cse417 ^ _cse409)) + 0xffffffffd3d89f33);
      _cse415 = ((((_cse416 & 0xfffe0000) >> 0x11) | (_cse416 << 0xf)) + _cse400);
      _cse414 = (((_cse363 + _cse409) + (_cse415 ^ _cse417)) + 0x15a2af88);
      _cse413 = ((((_cse414 & 0xffc00000) >> 0x16) | (_cse414 << 0xa)) + _cse415);
      _cse412 = (_cse413 ^ _cse415);
      _cse411 = (((_cse375 + _cse404) + (_cse412 ^ _cse400)) + 0xffffffff924dac27);
      _cse410 = ((((_cse411 & t590) >> 0x7) | (_cse411 << 0x19)) + _cse413);
      _cse398 = (((_cse399 + _cse400) + (_cse410 ^ _cse412)) + 0x7e563339);
      _cse397 = ((((_cse398 & t591) >> 0xc) | (_cse398 << 0x14)) + _cse410);
      _cse423 = (_cse397 ^ _cse410);
      _cse422 = (((_cse390 + _cse415) + (_cse423 ^ _cse413)) + t592);
      _cse421 = ((((_cse422 & 0xfffe0000) >> 0x11) | (_cse422 << 0xf)) + _cse397);
      _cse420 = (((_cse366 + _cse413) + (_cse421 ^ _cse423)) + t593);
      _cse419 = ((((_cse420 & 0xffc00000) >> 0x16) | (_cse420 << 0xa)) + _cse421);
      _cse418 = (_cse419 ^ _cse421);
      _cse425 = (((_cse378 + _cse410) + (_cse418 ^ _cse397)) + t594);
      _cse424 = ((((_cse425 & t590) >> 0x7) | (_cse425 << 0x19)) + _cse419);
      _cse395 = (((_cse396 + _cse397) + (_cse418 ^ _cse424)) + t595);
      _cse394 = ((((_cse395 & t591) >> 0xc) | (_cse395 << 0x14)) + _cse424);
      _cse429 = (((_cse387 + _cse421) + ((_cse424 ^ _cse419) ^ _cse394)) + 0x23e22dcc);
      _cse428 = ((((_cse429 & 0xfffe0000) >> 0x11) | (_cse429 << 0xf)) + _cse394);
      _cse427 = (((_cse357 + _cse419) + ((_cse394 ^ _cse424) ^ _cse428)) + t596);
      _cse426 = ((((_cse427 & 0xffc00000) >> 0x16) | (_cse427 << 0xa)) + _cse428);
      _cse431 = (((_cse369 + _cse424) + ((_cse428 ^ _cse394) ^ _cse426)) + t597);
      _cse430 = ((((_cse431 & t590) >> 0x7) | (_cse431 << 0x19)) + _cse426);
      _cse392 = (((_cse393 + _cse394) + ((_cse426 ^ _cse428) ^ _cse430)) + t598);
      _cse391 = ((((_cse392 & t591) >> 0xc) | (_cse392 << 0x14)) + _cse430);
      _cse437 = (((_cse384 + _cse428) + ((_cse430 ^ _cse426) ^ _cse391)) + t599);
      _cse436 = ((((_cse437 & 0xfffe0000) >> 0x11) | (_cse437 << 0xf)) + _cse391);
      _cse435 = (((_cse360 + _cse426) + ((_cse391 ^ _cse430) ^ _cse436)) + 0xffffffffaad28857);
      _cse434 = ((((_cse435 & 0xffc00000) >> 0x16) | (_cse435 << 0xa)) + _cse436);
      _cse433 = (((_cse402 + _cse430) + ((_cse434 & _cse391) | (_cse436 & (~(_cse434))))) + 0xffffffff8de587b3);
      _cse432 = ((((_cse433 & 0xffffffe0) >> 0x5) | (_cse433 << 0x1b)) + _cse434);
      _cse389 = (((_cse390 + _cse391) + ((_cse432 & _cse436) | (_cse434 & (~(_cse432))))) + t600);
      _cse388 = ((((_cse389 & t601) >> 0x9) | (_cse389 << 0x17)) + _cse432);
      _cse443 = (((_cse357 + _cse436) + ((_cse388 & _cse434) | (_cse432 & (~(_cse388))))) + t602);
      _cse442 = ((((_cse443 & 0xffffc000) >> 0xe) | (_cse443 << 0x12)) + _cse388);
      _cse441 = (((_cse372 + _cse434) + ((_cse442 & _cse432) | (_cse388 & (~(_cse442))))) + 0x5e211c2c);
      _cse440 = ((((_cse441 & 0xfff00000) >> 0x14) | (_cse441 << 0xc)) + _cse442);
      _cse439 = (((_cse399 + _cse432) + ((_cse440 & _cse388) | (_cse442 & (~(_cse440))))) + 0xffffffffac5e94b2);
      _cse438 = ((((_cse439 & 0xffffffe0) >> 0x5) | (_cse439 << 0x1b)) + _cse440);
      _cse386 = (((_cse387 + _cse388) + ((_cse438 & _cse442) | (_cse440 & (~(_cse438))))) - 0x58b835e);
      _cse385 = ((((_cse386 & t601) >> 0x9) | (_cse386 << 0x17)) + _cse438);
      _cse449 = (((_cse360 + _cse442) + ((_cse385 & _cse440) | (_cse438 & (~(_cse385))))) + 0x674b87b8);
      _cse448 = ((((_cse449 & 0xffffc000) >> 0xe) | (_cse449 << 0x12)) + _cse385);
      _cse447 = (((_cse375 + _cse440) + ((_cse448 & _cse438) | (_cse385 & (~(_cse448))))) + t603);
      _cse446 = ((((_cse447 & 0xfff00000) >> 0x14) | (_cse447 << 0xc)) + _cse448);
      _cse445 = (((_cse396 + _cse438) + ((_cse446 & _cse385) | (_cse448 & (~(_cse446))))) + t604);
      _cse444 = ((((_cse445 & 0xffffffe0) >> 0x5) | (_cse445 << 0x1b)) + _cse446);
      _cse383 = (((_cse384 + _cse385) + ((_cse444 & _cse448) | (_cse446 & (~(_cse444))))) + t605);
      _cse382 = ((((_cse383 & t601) >> 0x9) | (_cse383 << 0x17)) + _cse444);
      _cse455 = (((_cse363 + _cse448) + ((_cse382 & _cse446) | (_cse444 & (~(_cse382))))) + 0xffffffff91bf8078);
      _cse454 = ((((_cse455 & 0xffffc000) >> 0xe) | (_cse455 << 0x12)) + _cse382);
      _cse453 = (((_cse378 + _cse446) + ((_cse454 & _cse444) | (_cse382 & (~(_cse454))))) + t6);
      _cse452 = ((((_cse453 & 0xfff00000) >> 0x14) | (_cse453 << 0xc)) + _cse454);
      _cse451 = (((_cse393 + _cse444) + ((_cse452 & _cse382) | (_cse454 & (~(_cse452))))) + 0x7c24dd24);
      _cse450 = ((((_cse451 & 0xffffffe0) >> 0x5) | (_cse451 << 0x1b)) + _cse452);
      _cse380 = (((_cse381 + _cse382) + ((_cse450 & _cse454) | (_cse452 & (~(_cse450))))) + t606);
      _cse379 = ((((_cse380 & t601) >> 0x9) | (_cse380 << 0x17)) + _cse450);
      _cse462 = (~(_cse379));
      _cse461 = (((_cse366 + _cse454) + ((_cse379 & _cse452) | (_cse450 & _cse462))) + 0xffffffffd88f1be9);
      _cse460 = ((((_cse461 & 0xffffc000) >> 0xe) | (_cse461 << 0x12)) + _cse379);
      _cse463 = (~(_cse460));
      _cse459 = (((_cse369 + _cse452) + ((_cse460 & _cse450) | (_cse379 & _cse463))) + t5);
      _cse458 = ((((_cse459 & 0xfff00000) >> 0x14) | (_cse459 << 0xc)) + _cse460);
      _cse457 = (((_cse399 + _cse450) + ((_cse458 & _cse462) | (_cse460 & _cse379))) + 0xffffffff813c91ac);
      _cse456 = ((((_cse457 & t607) >> 0x4) | (_cse457 << 0x1c)) + _cse458);
      _cse377 = (((_cse378 + _cse379) + ((_cse456 & _cse463) | (_cse458 & _cse460))) + 0xffffffffcf6eb59d);
      _cse376 = ((((_cse377 & 0xfffff800) >> 0xb) | (_cse377 << 0x15)) + _cse456);
      _cse469 = (((_cse357 + _cse460) + ((_cse376 & (~(_cse458))) | (_cse456 & _cse458))) + 0xffffffffafdbd5e2);
      _cse468 = ((((_cse469 & 0xffff0000) >> 0x10) | (_cse469 << 0x10)) + _cse376);
      _cse467 = (((_cse384 + _cse458) + ((_cse468 & (~(_cse456))) | (_cse376 & _cse456))) + 0xffffffff80e7c964);
      _cse466 = ((((_cse467 & 0xff800000) >> 0x17) | (_cse467 << 0x9)) + _cse468);
      _cse465 = (((_cse402 + _cse456) + ((_cse466 & (~(_cse376))) | (_cse468 & _cse376))) + t608);
      _cse464 = ((((_cse465 & t607) >> 0x4) | (_cse465 << 0x1c)) + _cse466);
      _cse374 = (((_cse375 + _cse376) + ((_cse464 & (~(_cse468))) | (_cse466 & _cse468))) + t609);
      _cse373 = ((((_cse374 & 0xfffff800) >> 0xb) | (_cse374 << 0x15)) + _cse464);
      _cse475 = (((_cse366 + _cse468) + ((_cse373 & (~(_cse466))) | (_cse464 & _cse466))) + t610);
      _cse474 = ((((_cse475 & 0xffff0000) >> 0x10) | (_cse475 << 0x10)) + _cse373);
      _cse473 = (((_cse387 + _cse466) + ((_cse474 & (~(_cse464))) | (_cse373 & _cse464))) + t611);
      _cse472 = ((((_cse473 & 0xff800000) >> 0x17) | (_cse473 << 0x9)) + _cse474);
      _cse471 = (((_cse393 + _cse464) + ((_cse472 & (~(_cse373))) | (_cse474 & _cse373))) + t612);
      _cse470 = ((((_cse471 & t607) >> 0x4) | (_cse471 << 0x1c)) + _cse472);
      _cse371 = (((_cse372 + _cse373) + ((_cse470 & (~(_cse474))) | (_cse472 & _cse474))) + t613);
      _cse370 = ((((_cse371 & 0xfffff800) >> 0xb) | (_cse371 << 0x15)) + _cse470);
      _cse481 = (((_cse363 + _cse474) + ((_cse370 & (~(_cse472))) | (_cse470 & _cse472))) + t614);
      _cse480 = ((((_cse481 & 0xffff0000) >> 0x10) | (_cse481 << 0x10)) + _cse370);
      _cse479 = (((_cse390 + _cse472) + ((_cse480 & (~(_cse470))) | (_cse370 & _cse470))) + t615);
      _cse478 = ((((_cse479 & 0xff800000) >> 0x17) | (_cse479 << 0x9)) + _cse480);
      _cse477 = (((_cse396 + _cse470) + ((_cse478 & (~(_cse370))) | (_cse480 & _cse370))) + t616);
      _cse476 = ((((_cse477 & t607) >> 0x4) | (_cse477 << 0x1c)) + _cse478);
      _cse368 = (((_cse369 + _cse370) + ((_cse476 & (~(_cse480))) | (_cse478 & _cse480))) + t617);
      _cse367 = ((((_cse368 & 0xfffff800) >> 0xb) | (_cse368 << 0x15)) + _cse476);
      _cse483 = (((_cse360 + _cse480) + ((_cse367 & (~(_cse478))) | (_cse476 & _cse478))) + t618);
      _cse482 = ((((_cse483 & 0xffff0000) >> 0x10) | (_cse483 << 0x10)) + _cse367);
      _cse487 = (((_cse381 + _cse478) + ((_cse482 & (~(_cse476))) | (_cse367 & _cse476))) + t619);
      _cse486 = ((((_cse487 & 0xff800000) >> 0x17) | (_cse487 << 0x9)) + _cse482);
      _cse485 = (((_cse372 + _cse476) + ((_cse367 | (~(_cse486))) ^ _cse482)) + t620);
      _cse484 = ((((_cse485 & t621) >> 0x6) | (_cse485 << 0x1a)) + _cse486);
      _cse365 = (((_cse366 + _cse367) + ((_cse482 | (~(_cse484))) ^ _cse486)) + t622);
      _cse364 = ((((_cse365 & t623) >> 0xa) | (_cse365 << 0x16)) + _cse484);
      _cse489 = (((_cse384 + _cse482) + ((_cse486 | (~(_cse364))) ^ _cse484)) + t624);
      _cse488 = ((((_cse489 & t625) >> 0xf) | (_cse489 << 0x11)) + _cse364);
      _cse493 = (((_cse399 + _cse486) + ((_cse484 | (~(_cse488))) ^ _cse364)) + t626);
      _cse492 = ((((_cse493 & t627) >> 0x15) | (_cse493 << 0xb)) + _cse488);
      _cse491 = (((_cse369 + _cse484) + ((_cse364 | (~(_cse492))) ^ _cse488)) + t628);
      _cse490 = ((((_cse491 & t621) >> 0x6) | (_cse491 << 0x1a)) + _cse492);
      _cse362 = (((_cse363 + _cse364) + ((_cse488 | (~(_cse490))) ^ _cse492)) + t629);
      _cse361 = ((((_cse362 & t623) >> 0xa) | (_cse362 << 0x16)) + _cse490);
      _cse495 = (((_cse387 + _cse488) + ((_cse492 | (~(_cse361))) ^ _cse490)) + t630);
      _cse494 = ((((_cse495 & t625) >> 0xf) | (_cse495 << 0x11)) + _cse361);
      _cse499 = (((_cse402 + _cse492) + ((_cse490 | (~(_cse494))) ^ _cse361)) + t631);
      _cse498 = ((((_cse499 & t627) >> 0x15) | (_cse499 << 0xb)) + _cse494);
      _cse497 = (((_cse378 + _cse490) + ((_cse361 | (~(_cse498))) ^ _cse494)) + t632);
      _cse496 = ((((_cse497 & t621) >> 0x6) | (_cse497 << 0x1a)) + _cse498);
      _cse359 = (((_cse360 + _cse361) + ((_cse494 | (~(_cse496))) ^ _cse498)) + t633);
      _cse358 = ((((_cse359 & t623) >> 0xa) | (_cse359 << 0x16)) + _cse496);
      _cse501 = (((_cse390 + _cse494) + ((_cse498 | (~(_cse358))) ^ _cse496)) + t634);
      _cse500 = ((((_cse501 & t625) >> 0xf) | (_cse501 << 0x11)) + _cse358);
      _cse505 = (((_cse393 + _cse498) + ((_cse496 | (~(_cse500))) ^ _cse358)) + t635);
      _cse504 = ((((_cse505 & t627) >> 0x15) | (_cse505 << 0xb)) + _cse500);
      _cse503 = (((_cse375 + _cse496) + ((_cse358 | (~(_cse504))) ^ _cse500)) + t636);
      _cse502 = ((((_cse503 & t621) >> 0x6) | (_cse503 << 0x1a)) + _cse504);
      _cse356 = (((_cse357 + _cse358) + ((_cse500 | (~(_cse502))) ^ _cse504)) + t637);
      _cse355 = ((((_cse356 & t623) >> 0xa) | (_cse356 << 0x16)) + _cse502);
      _cse354 = (_cse355 + _cse406);
      (*var_740) = var_4f3;
      t638 = var_1ce8;
      var_4f0 = ((uint8_t)((_cse354 >> 0x8)));
      _cse506 = (_cse502 + _cse403);
      var_4ee = ((uint8_t)((_cse506 >> 0x8)));
      var_4ed = ((uint8_t)((_cse354 >> 0x10)));
      _cse509 = (((_cse381 + _cse500) + ((_cse504 | (~(_cse355))) ^ _cse502)) + t638);
      _cse508 = ((((_cse509 & t625) >> 0xf) | (_cse509 << 0x11)) + _cse355);
      _cse507 = (_cse508 + _cse408);
      var_1337 = ((uint8_t)((_cse507 >> 0x8)));
      _cse511 = (((_cse396 + _cse504) + ((_cse502 | (~(_cse508))) ^ _cse355)) + var_1cf0);
      _cse510 = ((_cse508 + _cse409) + (((_cse511 & t627) >> 0x15) | (_cse511 << 0xb)));
      var_4ef = ((uint8_t)((_cse510 >> 0x8)));
      var_4f2 = ((uint8_t)(_cse510));
      var_4f1 = ((uint8_t)(_cse506));
      (*(uint8_t*)(&var_1334)) = var_4f1;
      (*(uint8_t*)(&var_1338)) = ((uint8_t)(_cse507));
      (*(uint8_t*)(&var_133a)) = ((uint8_t)((_cse510 >> 0x10)));
      (*(uint8_t*)(&var_1336)) = ((uint8_t)((_cse507 >> 0x10)));
      var_4ec = ((uint8_t)(_cse512));
      _cse512 = (_cse506 >> 0x10);
      var_1332 = var_4ec;
      var_4eb = ((uint8_t)((_cse354 >> 0x18)));
      var_4ea = ((uint8_t)((_cse507 >> 0x18)));
      var_4e9 = ((uint8_t)(_cse513));
      _cse513 = (_cse506 >> 0x18);
      var_1331 = var_4e9;
      if ((((uint64_t)(var_1010) < (uint64_t)(var_740)) == 0x1)) {
        var_1364 = 0xffffffffb47a9ad4;
      } else {
        var_1364 = 0xfffffffff23572f0;
      }
      t0 = 0xffffffffb47a9ad4;
      _cse516 = (((((((int64_t)(int8_t)(t546)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t545)) & 0xff)) | ((((int64_t)(int8_t)(t547)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t548)) << 0x18));
      _cse519 = (((((((int64_t)(int8_t)(t562)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t561)) & 0xff)) | ((((int64_t)(int8_t)(t563)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t564)) << 0x18));
      _cse522 = (((((((int64_t)(int8_t)(t578)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t577)) & 0xff)) | ((((int64_t)(int8_t)(t579)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t580)) << 0x18));
      _cse525 = (((((((int64_t)(int8_t)(t530)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t529)) & 0xff)) | ((((int64_t)(int8_t)(t531)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t532)) << 0x18));
      _cse528 = (((((((int64_t)(int8_t)(t566)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t565)) & 0xff)) | ((((int64_t)(int8_t)(t567)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t568)) << 0x18));
      _cse531 = (((((((int64_t)(int8_t)(t582)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t581)) & 0xff)) | ((((int64_t)(int8_t)(t583)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t584)) << 0x18));
      _cse534 = (((((((int64_t)(int8_t)(t534)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t533)) & 0xff)) | ((((int64_t)(int8_t)(t535)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t536)) << 0x18));
      _cse537 = (((((((int64_t)(int8_t)(t550)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t549)) & 0xff)) | ((((int64_t)(int8_t)(t551)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t552)) << 0x18));
      _cse554 = ((int64_t)(int32_t)(t585));
      _cse556 = ((int64_t)(int32_t)(t587));
      _cse557 = ((int64_t)(int32_t)(t586));
      _cse555 = (_cse556 ^ _cse557);
      _cse558 = ((int64_t)(int32_t)(t588));
      _cse553 = (((_cse525 + _cse554) + (_cse555 ^ _cse558)) + t589);
      _cse552 = ((((_cse553 & t590) >> 0x7) | (_cse553 << 0x19)) + _cse557);
      _cse562 = (((((((int64_t)(int8_t)(t542)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t541)) & 0xff)) | ((((int64_t)(int8_t)(t543)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t544)) << 0x18));
      _cse565 = (((((((int64_t)(int8_t)(t538)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t537)) & 0xff)) | ((((int64_t)(int8_t)(t539)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t540)) << 0x18));
      _cse568 = (((_cse534 + _cse558) + (_cse552 ^ _cse555)) + 0x554e80f9);
      _cse567 = ((((_cse568 & t591) >> 0xc) | (_cse568 << 0x14)) + _cse552);
      _cse566 = (_cse567 ^ _cse552);
      _cse564 = (((_cse565 + _cse556) + (_cse566 ^ _cse557)) + 0xffffffffd3d89f33);
      _cse563 = ((((_cse564 & 0xfffe0000) >> 0x11) | (_cse564 << 0xf)) + _cse567);
      _cse561 = (((_cse562 + _cse557) + (_cse563 ^ _cse566)) + 0x15a2af88);
      _cse560 = ((((_cse561 & 0xffc00000) >> 0x16) | (_cse561 << 0xa)) + _cse563);
      _cse559 = (_cse560 ^ _cse563);
      _cse551 = (((_cse516 + _cse552) + (_cse559 ^ _cse567)) + 0xffffffff924dac27);
      _cse550 = ((((_cse551 & t590) >> 0x7) | (_cse551 << 0x19)) + _cse560);
      _cse572 = (((((((int64_t)(int8_t)(t558)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t557)) & 0xff)) | ((((int64_t)(int8_t)(t559)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t560)) << 0x18));
      _cse575 = (((((((int64_t)(int8_t)(t554)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t553)) & 0xff)) | ((((int64_t)(int8_t)(t555)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t556)) << 0x18));
      _cse578 = (((_cse537 + _cse567) + (_cse550 ^ _cse559)) + 0x7e563339);
      _cse577 = ((((_cse578 & t591) >> 0xc) | (_cse578 << 0x14)) + _cse550);
      _cse576 = (_cse577 ^ _cse550);
      _cse574 = (((_cse575 + _cse563) + (_cse576 ^ _cse560)) + t592);
      _cse573 = ((((_cse574 & 0xfffe0000) >> 0x11) | (_cse574 << 0xf)) + _cse577);
      _cse571 = (((_cse572 + _cse560) + (_cse573 ^ _cse576)) + t593);
      _cse570 = ((((_cse571 & 0xffc00000) >> 0x16) | (_cse571 << 0xa)) + _cse573);
      _cse569 = (_cse570 ^ _cse573);
      _cse549 = (((_cse519 + _cse550) + (_cse569 ^ _cse577)) + t594);
      _cse548 = ((((_cse549 & t590) >> 0x7) | (_cse549 << 0x19)) + _cse570);
      _cse581 = (((((((int64_t)(int8_t)(t570)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t569)) & 0xff)) | ((((int64_t)(int8_t)(t571)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t572)) << 0x18));
      _cse583 = (((_cse528 + _cse577) + (_cse569 ^ _cse548)) + t595);
      _cse582 = ((((_cse583 & t591) >> 0xc) | (_cse583 << 0x14)) + _cse548);
      _cse580 = (((_cse581 + _cse573) + ((_cse548 ^ _cse570) ^ _cse582)) + 0x23e22dcc);
      _cse579 = ((((_cse580 & 0xfffe0000) >> 0x11) | (_cse580 << 0xf)) + _cse582);
      _cse586 = (((((((int64_t)(int8_t)(t574)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t573)) & 0xff)) | ((((int64_t)(int8_t)(t575)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t576)) << 0x18));
      _cse585 = (((_cse586 + _cse570) + ((_cse582 ^ _cse548) ^ _cse579)) + t596);
      _cse584 = ((((_cse585 & 0xffc00000) >> 0x16) | (_cse585 << 0xa)) + _cse579);
      _cse547 = (((_cse522 + _cse548) + ((_cse579 ^ _cse582) ^ _cse584)) + t597);
      _cse546 = ((((_cse547 & t590) >> 0x7) | (_cse547 << 0x19)) + _cse584);
      _cse590 = (t526 + ((((int64_t)(int32_t)(t524)) << 0x3) & t525));
      _cse589 = (((((_cse590 >> 0x30) & 0xff) | ((_cse590 >> 0x38) << 0x18)) | ((_cse590 >> 0x20) & t527)) | ((_cse590 >> 0x10) & t528));
      _cse592 = (((_cse531 + _cse582) + ((_cse584 ^ _cse579) ^ _cse546)) + t598);
      _cse591 = ((((_cse592 & t591) >> 0xc) | (_cse592 << 0x14)) + _cse546);
      _cse596 = (_cse590 ^ 0x64130efb);
      _cse595 = ((((_cse596 >> 0x10) & 0xff) | (_cse596 & 0xffffffffff00ff00)) | ((_cse596 << 0x10) & t528));
      _cse594 = (((_cse595 + _cse579) + ((_cse546 ^ _cse584) ^ _cse591)) + t599);
      _cse593 = ((((_cse594 & 0xfffe0000) >> 0x11) | (_cse594 << 0xf)) + _cse591);
      _cse588 = (((_cse589 + _cse584) + ((_cse591 ^ _cse546) ^ _cse593)) + 0xffffffffaad28857);
      _cse587 = ((((_cse588 & 0xffc00000) >> 0x16) | (_cse588 << 0xa)) + _cse593);
      _cse545 = (((_cse534 + _cse546) + ((_cse587 & _cse591) | (_cse593 & (~(_cse587))))) + 0xffffffff8de587b3);
      _cse544 = ((((_cse545 & 0xffffffe0) >> 0x5) | (_cse545 << 0x1b)) + _cse587);
      _cse602 = (((_cse575 + _cse591) + ((_cse544 & _cse593) | (_cse587 & (~(_cse544))))) + t600);
      _cse601 = ((((_cse602 & t601) >> 0x9) | (_cse602 << 0x17)) + _cse544);
      _cse600 = (((_cse586 + _cse593) + ((_cse601 & _cse587) | (_cse544 & (~(_cse601))))) + t602);
      _cse599 = ((((_cse600 & 0xffffc000) >> 0xe) | (_cse600 << 0x12)) + _cse601);
      _cse598 = (((_cse525 + _cse587) + ((_cse599 & _cse544) | (_cse601 & (~(_cse599))))) + 0x5e211c2c);
      _cse597 = ((((_cse598 & 0xfff00000) >> 0x14) | (_cse598 << 0xc)) + _cse599);
      _cse543 = (((_cse537 + _cse544) + ((_cse597 & _cse601) | (_cse599 & (~(_cse597))))) + 0xffffffffac5e94b2);
      _cse542 = ((((_cse543 & 0xffffffe0) >> 0x5) | (_cse543 << 0x1b)) + _cse597);
      _cse608 = (((_cse581 + _cse601) + ((_cse542 & _cse599) | (_cse597 & (~(_cse542))))) - 0x58b835e);
      _cse607 = ((((_cse608 & t601) >> 0x9) | (_cse608 << 0x17)) + _cse542);
      _cse606 = (((_cse589 + _cse599) + ((_cse607 & _cse597) | (_cse542 & (~(_cse607))))) + 0x674b87b8);
      _cse605 = ((((_cse606 & 0xffffc000) >> 0xe) | (_cse606 << 0x12)) + _cse607);
      _cse604 = (((_cse516 + _cse597) + ((_cse605 & _cse542) | (_cse607 & (~(_cse605))))) + t603);
      _cse603 = ((((_cse604 & 0xfff00000) >> 0x14) | (_cse604 << 0xc)) + _cse605);
      _cse541 = (((_cse528 + _cse542) + ((_cse603 & _cse607) | (_cse605 & (~(_cse603))))) + t604);
      _cse540 = ((((_cse541 & 0xffffffe0) >> 0x5) | (_cse541 << 0x1b)) + _cse603);
      _cse614 = (((_cse595 + _cse607) + ((_cse540 & _cse605) | (_cse603 & (~(_cse540))))) + t605);
      _cse613 = ((((_cse614 & t601) >> 0x9) | (_cse614 << 0x17)) + _cse540);
      _cse612 = (((_cse562 + _cse605) + ((_cse613 & _cse603) | (_cse540 & (~(_cse613))))) + 0xffffffff91bf8078);
      _cse611 = ((((_cse612 & 0xffffc000) >> 0xe) | (_cse612 << 0x12)) + _cse613);
      _cse610 = (((_cse519 + _cse603) + ((_cse611 & _cse540) | (_cse613 & (~(_cse611))))) + t6);
      _cse609 = ((((_cse610 & 0xfff00000) >> 0x14) | (_cse610 << 0xc)) + _cse611);
      _cse539 = (((_cse531 + _cse540) + ((_cse609 & _cse613) | (_cse611 & (~(_cse609))))) + 0x7c24dd24);
      _cse538 = ((((_cse539 & 0xffffffe0) >> 0x5) | (_cse539 << 0x1b)) + _cse609);
      _cse620 = (((_cse565 + _cse613) + ((_cse538 & _cse611) | (_cse609 & (~(_cse538))))) + t606);
      _cse619 = ((((_cse620 & t601) >> 0x9) | (_cse620 << 0x17)) + _cse538);
      _cse621 = (~(_cse619));
      _cse618 = (((_cse572 + _cse611) + ((_cse619 & _cse609) | (_cse538 & _cse621))) + 0xffffffffd88f1be9);
      _cse617 = ((((_cse618 & 0xffffc000) >> 0xe) | (_cse618 << 0x12)) + _cse619);
      _cse622 = (~(_cse617));
      _cse616 = (((_cse522 + _cse609) + ((_cse617 & _cse538) | (_cse619 & _cse622))) + t5);
      _cse615 = ((((_cse616 & 0xfff00000) >> 0x14) | (_cse616 << 0xc)) + _cse617);
      _cse536 = (((_cse537 + _cse538) + ((_cse615 & _cse621) | (_cse617 & _cse619))) + 0xffffffff813c91ac);
      _cse535 = ((((_cse536 & t607) >> 0x4) | (_cse536 << 0x1c)) + _cse615);
      _cse628 = (((_cse519 + _cse619) + ((_cse535 & _cse622) | (_cse615 & _cse617))) + 0xffffffffcf6eb59d);
      _cse627 = ((((_cse628 & 0xfffff800) >> 0xb) | (_cse628 << 0x15)) + _cse535);
      _cse626 = (((_cse586 + _cse617) + ((_cse627 & (~(_cse615))) | (_cse535 & _cse615))) + 0xffffffffafdbd5e2);
      _cse625 = ((((_cse626 & 0xffff0000) >> 0x10) | (_cse626 << 0x10)) + _cse627);
      _cse624 = (((_cse595 + _cse615) + ((_cse625 & (~(_cse535))) | (_cse627 & _cse535))) + 0xffffffff80e7c964);
      _cse623 = ((((_cse624 & 0xff800000) >> 0x17) | (_cse624 << 0x9)) + _cse625);
      _cse533 = (((_cse534 + _cse535) + ((_cse623 & (~(_cse627))) | (_cse625 & _cse627))) + t608);
      _cse532 = ((((_cse533 & t607) >> 0x4) | (_cse533 << 0x1c)) + _cse623);
      _cse634 = (((_cse516 + _cse627) + ((_cse532 & (~(_cse625))) | (_cse623 & _cse625))) + t609);
      _cse633 = ((((_cse634 & 0xfffff800) >> 0xb) | (_cse634 << 0x15)) + _cse532);
      _cse632 = (((_cse572 + _cse625) + ((_cse633 & (~(_cse623))) | (_cse532 & _cse623))) + t610);
      _cse631 = ((((_cse632 & 0xffff0000) >> 0x10) | (_cse632 << 0x10)) + _cse633);
      _cse630 = (((_cse581 + _cse623) + ((_cse631 & (~(_cse532))) | (_cse633 & _cse532))) + t611);
      _cse629 = ((((_cse630 & 0xff800000) >> 0x17) | (_cse630 << 0x9)) + _cse631);
      _cse530 = (((_cse531 + _cse532) + ((_cse629 & (~(_cse633))) | (_cse631 & _cse633))) + t612);
      _cse529 = ((((_cse530 & t607) >> 0x4) | (_cse530 << 0x1c)) + _cse629);
      _cse640 = (((_cse525 + _cse633) + ((_cse529 & (~(_cse631))) | (_cse629 & _cse631))) + t613);
      _cse639 = ((((_cse640 & 0xfffff800) >> 0xb) | (_cse640 << 0x15)) + _cse529);
      _cse638 = (((_cse562 + _cse631) + ((_cse639 & (~(_cse629))) | (_cse529 & _cse629))) + t614);
      _cse637 = ((((_cse638 & 0xffff0000) >> 0x10) | (_cse638 << 0x10)) + _cse639);
      _cse636 = (((_cse575 + _cse629) + ((_cse637 & (~(_cse529))) | (_cse639 & _cse529))) + t615);
      _cse635 = ((((_cse636 & 0xff800000) >> 0x17) | (_cse636 << 0x9)) + _cse637);
      _cse527 = (((_cse528 + _cse529) + ((_cse635 & (~(_cse639))) | (_cse637 & _cse639))) + t616);
      _cse526 = ((((_cse527 & t607) >> 0x4) | (_cse527 << 0x1c)) + _cse635);
      _cse642 = (((_cse522 + _cse639) + ((_cse526 & (~(_cse637))) | (_cse635 & _cse637))) + t617);
      _cse641 = ((((_cse642 & 0xfffff800) >> 0xb) | (_cse642 << 0x15)) + _cse526);
      _cse646 = (((_cse589 + _cse637) + ((_cse641 & (~(_cse635))) | (_cse526 & _cse635))) + t618);
      _cse645 = ((((_cse646 & 0xffff0000) >> 0x10) | (_cse646 << 0x10)) + _cse641);
      _cse644 = (((_cse565 + _cse635) + ((_cse645 & (~(_cse526))) | (_cse641 & _cse526))) + t619);
      _cse643 = ((((_cse644 & 0xff800000) >> 0x17) | (_cse644 << 0x9)) + _cse645);
      _cse524 = (((_cse525 + _cse526) + ((_cse641 | (~(_cse643))) ^ _cse645)) + t620);
      _cse523 = ((((_cse524 & t621) >> 0x6) | (_cse524 << 0x1a)) + _cse643);
      _cse648 = (((_cse572 + _cse641) + ((_cse645 | (~(_cse523))) ^ _cse643)) + t622);
      _cse647 = ((((_cse648 & t623) >> 0xa) | (_cse648 << 0x16)) + _cse523);
      _cse652 = (((_cse595 + _cse645) + ((_cse643 | (~(_cse647))) ^ _cse523)) + t624);
      _cse651 = ((((_cse652 & t625) >> 0xf) | (_cse652 << 0x11)) + _cse647);
      _cse650 = (((_cse537 + _cse643) + ((_cse523 | (~(_cse651))) ^ _cse647)) + t626);
      _cse649 = ((((_cse650 & t627) >> 0x15) | (_cse650 << 0xb)) + _cse651);
      _cse521 = (((_cse522 + _cse523) + ((_cse647 | (~(_cse649))) ^ _cse651)) + t628);
      _cse520 = ((((_cse521 & t621) >> 0x6) | (_cse521 << 0x1a)) + _cse649);
      _cse654 = (((_cse562 + _cse647) + ((_cse651 | (~(_cse520))) ^ _cse649)) + t629);
      _cse653 = ((((_cse654 & t623) >> 0xa) | (_cse654 << 0x16)) + _cse520);
      _cse658 = (((_cse581 + _cse651) + ((_cse649 | (~(_cse653))) ^ _cse520)) + t630);
      _cse657 = ((((_cse658 & t625) >> 0xf) | (_cse658 << 0x11)) + _cse653);
      _cse656 = (((_cse534 + _cse649) + ((_cse520 | (~(_cse657))) ^ _cse653)) + t631);
      _cse655 = ((((_cse656 & t627) >> 0x15) | (_cse656 << 0xb)) + _cse657);
      _cse518 = (((_cse519 + _cse520) + ((_cse653 | (~(_cse655))) ^ _cse657)) + t632);
      _cse517 = ((((_cse518 & t621) >> 0x6) | (_cse518 << 0x1a)) + _cse655);
      _cse660 = (((_cse589 + _cse653) + ((_cse657 | (~(_cse517))) ^ _cse655)) + t633);
      _cse659 = ((((_cse660 & t623) >> 0xa) | (_cse660 << 0x16)) + _cse517);
      _cse664 = (((_cse575 + _cse657) + ((_cse655 | (~(_cse659))) ^ _cse517)) + t634);
      _cse663 = ((((_cse664 & t625) >> 0xf) | (_cse664 << 0x11)) + _cse659);
      _cse662 = (((_cse531 + _cse655) + ((_cse517 | (~(_cse663))) ^ _cse659)) + t635);
      _cse661 = ((((_cse662 & t627) >> 0x15) | (_cse662 << 0xb)) + _cse663);
      _cse515 = (((_cse516 + _cse517) + ((_cse659 | (~(_cse661))) ^ _cse663)) + t636);
      _cse514 = ((((_cse515 & t621) >> 0x6) | (_cse515 << 0x1a)) + _cse661);
      t1 = (_cse514 + _cse558);
      _cse667 = (((_cse586 + _cse659) + ((_cse663 | (~(_cse514))) ^ _cse661)) + t637);
      _cse666 = ((((_cse667 & t623) >> 0xa) | (_cse667 << 0x16)) + _cse514);
      _cse665 = (((_cse565 + _cse663) + ((_cse661 | (~(_cse666))) ^ _cse514)) + t638);
      t2 = (((((_cse665 & t625) >> 0xf) | (_cse665 << 0x11)) + _cse666) + _cse556);
      _cse668 = (_cse666 + _cse554);
      t3 = _cse668;
      t4 = (_cse668 >> 0x10);
      goto L_0x10004d40;
    }
    // b26 @0x10005228
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x1a490433) == 0x1)) {
      // b277 @0x1000a79c
      t306 = var_3ac;
      var_508 = (((int64_t)(int32_t)(t306)) & 0xffffffff);
      (*(uint8_t*)(((__entry_r125 - 0x1250) + var_508))) = 0xda;
      if (((var_508 < 0x38) == 0x1)) {
        var_1364 = 0xfffffffff40434cd;
      } else {
        var_1364 = 0x4ca4ff79;
      }
      var_500 = (var_508 + 0x1);
      goto L_0x10004d40;
    }
    // b28 @0x10005244
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x1ec32ac4) == 0x1)) {
      // b278 @0x1000a814
      t307 = (*var_a00);
      var_7c4 = t307;
      t308 = (*var_9f8);
      var_7c0 = t308;
      t309 = (*var_9f0);
      var_7bc = t309;
      t310 = (*var_9e8);
      var_7b8 = t310;
      t311 = (*var_9e0);
      var_7b4 = t311;
      t312 = (*var_9d8);
      var_7b0 = t312;
      t313 = (*var_9d0);
      var_7ac = t313;
      t314 = (*var_9c8);
      var_7a8 = t314;
      var_1364 = 0x965e0878;
      var_19c8 = var_1a98;
      var_19d4 = ((uint32_t)(var_1a98));
      var_19cc = t307;
      var_19d0 = t308;
      var_19e8 = t310;
      var_19e4 = t311;
      var_19e0 = t312;
      var_19dc = t313;
      var_19d8 = t314;
      var_19ec = t309;
      goto L_0x10004d40;
    }
    // b30 @0x10005260
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x1ff94f5f) == 0x1)) {
      // b32 @0x1000527c
      t40 = (*var_700);
      t41 = (*var_6f8);
      t42 = (*var_738);
      t43 = (*var_6f0);
      t44 = (*var_6e8);
      t45 = (*var_6e0);
      t46 = (*var_6d8);
      t47 = (*var_6d0);
      t48 = (*var_6c8);
      t49 = (*var_6c0);
      t50 = (*var_6b8);
      t51 = (*var_6b0);
      t52 = (*var_6a8);
      t53 = (*var_6a0);
      t54 = (*var_698);
      t55 = (*var_690);
      t56 = (*var_688);
      t57 = (*var_680);
      t58 = (*var_678);
      t59 = (*var_670);
      t60 = (*var_668);
      t61 = (*var_660);
      t62 = (*var_658);
      t63 = (*var_650);
      t64 = (*var_648);
      t65 = (*var_640);
      t66 = (*var_638);
      t67 = (*var_630);
      t68 = (*var_628);
      t69 = (*var_620);
      t70 = (*var_618);
      t71 = (*var_610);
      t72 = (*var_608);
      t73 = (*var_600);
      t74 = (*var_5f8);
      t75 = (*var_5f0);
      t76 = (*var_5e8);
      t77 = (*var_5e0);
      t78 = (*var_5d8);
      t79 = (*var_5d0);
      t80 = (*var_5c8);
      t81 = (*var_5c0);
      t82 = (*var_5b8);
      t83 = (*var_5b0);
      t84 = (*var_5a8);
      t85 = (*var_5a0);
      t86 = (*var_598);
      t87 = (*var_590);
      t88 = (*var_588);
      t89 = (*var_580);
      t90 = (*var_578);
      t91 = (*var_570);
      t92 = (*var_568);
      t93 = (*var_560);
      t94 = (*var_558);
      t95 = (*var_550);
      t96 = (*var_548);
      t97 = (*var_540);
      t98 = (*var_538);
      t99 = (*var_530);
      t100 = (*var_528);
      t101 = (*var_520);
      t102 = (*var_518);
      t103 = (*var_510);
      t104 = var_720;
      t105 = (*(uint32_t*)(t104));
      t106 = (*var_718);
      t107 = (*var_710);
      t108 = (*var_708);
      t109 = var_1b68;
      t110 = var_1ac8;
      t111 = var_1be8;
      t112 = var_1b80;
      t113 = var_1b88;
      t114 = var_1b90;
      t115 = var_1b98;
      t116 = var_1ba0;
      t117 = var_1ba8;
      t118 = var_1bb0;
      t119 = var_1bb8;
      t120 = var_1bc8;
      t121 = var_1bf0;
      t122 = var_1bd0;
      t123 = var_1bd8;
      t124 = var_1b40;
      t125 = var_1b38;
      t126 = var_1be0;
      t127 = var_1ad8;
      t128 = var_1bf8;
      t129 = var_1c00;
      t130 = var_1c08;
      t131 = var_1c10;
      t132 = var_1c18;
      t133 = var_1c20;
      t134 = var_1c28;
      t135 = var_1c30;
      t136 = var_1c38;
      t137 = var_1c40;
      t138 = var_1c48;
      t139 = var_1c50;
      t140 = var_1c58;
      t141 = var_1c60;
      t142 = var_1c68;
      t143 = var_1c70;
      t144 = var_1c78;
      t145 = var_1c80;
      t146 = var_1c88;
      t147 = var_1c90;
      t148 = var_1c98;
      t149 = var_1ca0;
      t150 = var_1ca8;
      t151 = var_1cb0;
      t152 = var_1cb8;
      t153 = var_1cc0;
      t154 = var_1cc8;
      t155 = var_1cd0;
      t156 = var_1cd8;
      t157 = var_1ce0;
      _cse673 = (((((((int64_t)(int8_t)(t85)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t84)) & 0xff)) | ((((int64_t)(int8_t)(t86)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t87)) << 0x18));
      _cse676 = (((((((int64_t)(int8_t)(t101)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t100)) & 0xff)) | ((((int64_t)(int8_t)(t102)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t103)) << 0x18));
      _cse679 = (((((((int64_t)(int8_t)(t53)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t52)) & 0xff)) | ((((int64_t)(int8_t)(t54)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t55)) << 0x18));
      _cse682 = (((((((int64_t)(int8_t)(t69)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t68)) & 0xff)) | ((((int64_t)(int8_t)(t70)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t71)) << 0x18));
      _cse685 = (((((((int64_t)(int8_t)(t89)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t88)) & 0xff)) | ((((int64_t)(int8_t)(t90)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t91)) << 0x18));
      _cse688 = (((((((int64_t)(int8_t)(t41)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t40)) & 0xff)) | ((((int64_t)(int8_t)(t42)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t43)) << 0x18));
      _cse691 = (((((((int64_t)(int8_t)(t57)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t56)) & 0xff)) | ((((int64_t)(int8_t)(t58)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t59)) << 0x18));
      _cse694 = (((((((int64_t)(int8_t)(t73)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t72)) & 0xff)) | ((((int64_t)(int8_t)(t74)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t75)) << 0x18));
      _cse697 = (((((((int64_t)(int8_t)(t49)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t48)) & 0xff)) | ((((int64_t)(int8_t)(t50)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t51)) << 0x18));
      _cse700 = (((((((int64_t)(int8_t)(t97)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t96)) & 0xff)) | ((((int64_t)(int8_t)(t98)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t99)) << 0x18));
      _cse703 = (((((((int64_t)(int8_t)(t81)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t80)) & 0xff)) | ((((int64_t)(int8_t)(t82)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t83)) << 0x18));
      _cse706 = (((((((int64_t)(int8_t)(t65)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t64)) & 0xff)) | ((((int64_t)(int8_t)(t66)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t67)) << 0x18));
      _cse709 = (((((((int64_t)(int8_t)(t93)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t92)) & 0xff)) | ((((int64_t)(int8_t)(t94)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t95)) << 0x18));
      _cse712 = (((((((int64_t)(int8_t)(t77)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t76)) & 0xff)) | ((((int64_t)(int8_t)(t78)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t79)) << 0x18));
      _cse715 = (((((((int64_t)(int8_t)(t61)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t60)) & 0xff)) | ((((int64_t)(int8_t)(t62)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t63)) << 0x18));
      _cse718 = (((((((int64_t)(int8_t)(t45)) & 0xff) << 0x8) | (((int64_t)(int8_t)(t44)) & 0xff)) | ((((int64_t)(int8_t)(t46)) & 0xff) << 0x10)) | (((int64_t)(int8_t)(t47)) << 0x18));
      _cse719 = ((int64_t)(int32_t)(t108));
      _cse722 = ((int64_t)(int32_t)(t105));
      _cse724 = ((int64_t)(int32_t)(t107));
      _cse725 = ((int64_t)(int32_t)(t106));
      _cse723 = (_cse724 ^ _cse725);
      _cse721 = (((_cse688 + _cse722) + (_cse723 ^ _cse719)) + t109);
      _cse720 = ((((_cse721 & t110) >> 0x7) | (_cse721 << 0x19)) + _cse725);
      _cse717 = (((_cse718 + _cse719) + (_cse720 ^ _cse723)) + 0x554e80f9);
      _cse716 = ((((_cse717 & t111) >> 0xc) | (_cse717 << 0x14)) + _cse720);
      _cse733 = (_cse716 ^ _cse720);
      _cse732 = (((_cse697 + _cse724) + (_cse733 ^ _cse725)) + 0xffffffffd3d89f33);
      _cse731 = ((((_cse732 & 0xfffe0000) >> 0x11) | (_cse732 << 0xf)) + _cse716);
      _cse730 = (((_cse679 + _cse725) + (_cse731 ^ _cse733)) + 0x15a2af88);
      _cse729 = ((((_cse730 & 0xffc00000) >> 0x16) | (_cse730 << 0xa)) + _cse731);
      _cse728 = (_cse729 ^ _cse731);
      _cse727 = (((_cse691 + _cse720) + (_cse728 ^ _cse716)) + 0xffffffff924dac27);
      _cse726 = ((((_cse727 & t110) >> 0x7) | (_cse727 << 0x19)) + _cse729);
      _cse714 = (((_cse715 + _cse716) + (_cse726 ^ _cse728)) + 0x7e563339);
      _cse713 = ((((_cse714 & t111) >> 0xc) | (_cse714 << 0x14)) + _cse726);
      _cse739 = (_cse713 ^ _cse726);
      _cse738 = (((_cse706 + _cse731) + (_cse739 ^ _cse729)) + t112);
      _cse737 = ((((_cse738 & 0xfffe0000) >> 0x11) | (_cse738 << 0xf)) + _cse713);
      _cse736 = (((_cse682 + _cse729) + (_cse737 ^ _cse739)) + t113);
      _cse735 = ((((_cse736 & 0xffc00000) >> 0x16) | (_cse736 << 0xa)) + _cse737);
      _cse734 = (_cse735 ^ _cse737);
      _cse741 = (((_cse694 + _cse726) + (_cse734 ^ _cse713)) + t114);
      _cse740 = ((((_cse741 & t110) >> 0x7) | (_cse741 << 0x19)) + _cse735);
      _cse711 = (((_cse712 + _cse713) + (_cse734 ^ _cse740)) + t115);
      _cse710 = ((((_cse711 & t111) >> 0xc) | (_cse711 << 0x14)) + _cse740);
      _cse745 = (((_cse703 + _cse737) + ((_cse740 ^ _cse735) ^ _cse710)) + 0x23e22dcc);
      _cse744 = ((((_cse745 & 0xfffe0000) >> 0x11) | (_cse745 << 0xf)) + _cse710);
      _cse743 = (((_cse673 + _cse735) + ((_cse710 ^ _cse740) ^ _cse744)) + t116);
      _cse742 = ((((_cse743 & 0xffc00000) >> 0x16) | (_cse743 << 0xa)) + _cse744);
      _cse747 = (((_cse685 + _cse740) + ((_cse744 ^ _cse710) ^ _cse742)) + t117);
      _cse746 = ((((_cse747 & t110) >> 0x7) | (_cse747 << 0x19)) + _cse742);
      _cse708 = (((_cse709 + _cse710) + ((_cse742 ^ _cse744) ^ _cse746)) + t118);
      _cse707 = ((((_cse708 & t111) >> 0xc) | (_cse708 << 0x14)) + _cse746);
      _cse753 = (((_cse700 + _cse744) + ((_cse746 ^ _cse742) ^ _cse707)) + t119);
      _cse752 = ((((_cse753 & 0xfffe0000) >> 0x11) | (_cse753 << 0xf)) + _cse707);
      _cse751 = (((_cse676 + _cse742) + ((_cse707 ^ _cse746) ^ _cse752)) + 0xffffffffaad28857);
      _cse750 = ((((_cse751 & 0xffc00000) >> 0x16) | (_cse751 << 0xa)) + _cse752);
      _cse749 = (((_cse718 + _cse746) + ((_cse750 & _cse707) | (_cse752 & (~(_cse750))))) + 0xffffffff8de587b3);
      _cse748 = ((((_cse749 & 0xffffffe0) >> 0x5) | (_cse749 << 0x1b)) + _cse750);
      _cse705 = (((_cse706 + _cse707) + ((_cse748 & _cse752) | (_cse750 & (~(_cse748))))) + t120);
      _cse704 = ((((_cse705 & t121) >> 0x9) | (_cse705 << 0x17)) + _cse748);
      _cse759 = (((_cse673 + _cse752) + ((_cse704 & _cse750) | (_cse748 & (~(_cse704))))) + t122);
      _cse758 = ((((_cse759 & 0xffffc000) >> 0xe) | (_cse759 << 0x12)) + _cse704);
      _cse757 = (((_cse688 + _cse750) + ((_cse758 & _cse748) | (_cse704 & (~(_cse758))))) + 0x5e211c2c);
      _cse756 = ((((_cse757 & 0xfff00000) >> 0x14) | (_cse757 << 0xc)) + _cse758);
      _cse755 = (((_cse715 + _cse748) + ((_cse756 & _cse704) | (_cse758 & (~(_cse756))))) + 0xffffffffac5e94b2);
      _cse754 = ((((_cse755 & 0xffffffe0) >> 0x5) | (_cse755 << 0x1b)) + _cse756);
      _cse702 = (((_cse703 + _cse704) + ((_cse754 & _cse758) | (_cse756 & (~(_cse754))))) - 0x58b835e);
      _cse701 = ((((_cse702 & t121) >> 0x9) | (_cse702 << 0x17)) + _cse754);
      _cse765 = (((_cse676 + _cse758) + ((_cse701 & _cse756) | (_cse754 & (~(_cse701))))) + 0x674b87b8);
      _cse764 = ((((_cse765 & 0xffffc000) >> 0xe) | (_cse765 << 0x12)) + _cse701);
      _cse763 = (((_cse691 + _cse756) + ((_cse764 & _cse754) | (_cse701 & (~(_cse764))))) + t123);
      _cse762 = ((((_cse763 & 0xfff00000) >> 0x14) | (_cse763 << 0xc)) + _cse764);
      _cse761 = (((_cse712 + _cse754) + ((_cse762 & _cse701) | (_cse764 & (~(_cse762))))) + t124);
      _cse760 = ((((_cse761 & 0xffffffe0) >> 0x5) | (_cse761 << 0x1b)) + _cse762);
      _cse699 = (((_cse700 + _cse701) + ((_cse760 & _cse764) | (_cse762 & (~(_cse760))))) + t125);
      _cse698 = ((((_cse699 & t121) >> 0x9) | (_cse699 << 0x17)) + _cse760);
      _cse771 = (((_cse679 + _cse764) + ((_cse698 & _cse762) | (_cse760 & (~(_cse698))))) + 0xffffffff91bf8078);
      _cse770 = ((((_cse771 & 0xffffc000) >> 0xe) | (_cse771 << 0x12)) + _cse698);
      _cse769 = (((_cse694 + _cse762) + ((_cse770 & _cse760) | (_cse698 & (~(_cse770))))) + t6);
      _cse768 = ((((_cse769 & 0xfff00000) >> 0x14) | (_cse769 << 0xc)) + _cse770);
      _cse767 = (((_cse709 + _cse760) + ((_cse768 & _cse698) | (_cse770 & (~(_cse768))))) + 0x7c24dd24);
      _cse766 = ((((_cse767 & 0xffffffe0) >> 0x5) | (_cse767 << 0x1b)) + _cse768);
      _cse696 = (((_cse697 + _cse698) + ((_cse766 & _cse770) | (_cse768 & (~(_cse766))))) + t126);
      _cse695 = ((((_cse696 & t121) >> 0x9) | (_cse696 << 0x17)) + _cse766);
      _cse778 = (~(_cse695));
      _cse777 = (((_cse682 + _cse770) + ((_cse695 & _cse768) | (_cse766 & _cse778))) + 0xffffffffd88f1be9);
      _cse776 = ((((_cse777 & 0xffffc000) >> 0xe) | (_cse777 << 0x12)) + _cse695);
      _cse779 = (~(_cse776));
      _cse775 = (((_cse685 + _cse768) + ((_cse776 & _cse766) | (_cse695 & _cse779))) + t5);
      _cse774 = ((((_cse775 & 0xfff00000) >> 0x14) | (_cse775 << 0xc)) + _cse776);
      _cse773 = (((_cse715 + _cse766) + ((_cse774 & _cse778) | (_cse776 & _cse695))) + 0xffffffff813c91ac);
      _cse772 = ((((_cse773 & t127) >> 0x4) | (_cse773 << 0x1c)) + _cse774);
      _cse693 = (((_cse694 + _cse695) + ((_cse772 & _cse779) | (_cse774 & _cse776))) + 0xffffffffcf6eb59d);
      _cse692 = ((((_cse693 & 0xfffff800) >> 0xb) | (_cse693 << 0x15)) + _cse772);
      _cse785 = (((_cse673 + _cse776) + ((_cse692 & (~(_cse774))) | (_cse772 & _cse774))) + 0xffffffffafdbd5e2);
      _cse784 = ((((_cse785 & 0xffff0000) >> 0x10) | (_cse785 << 0x10)) + _cse692);
      _cse783 = (((_cse700 + _cse774) + ((_cse784 & (~(_cse772))) | (_cse692 & _cse772))) + 0xffffffff80e7c964);
      _cse782 = ((((_cse783 & 0xff800000) >> 0x17) | (_cse783 << 0x9)) + _cse784);
      _cse781 = (((_cse718 + _cse772) + ((_cse782 & (~(_cse692))) | (_cse784 & _cse692))) + t128);
      _cse780 = ((((_cse781 & t127) >> 0x4) | (_cse781 << 0x1c)) + _cse782);
      _cse690 = (((_cse691 + _cse692) + ((_cse780 & (~(_cse784))) | (_cse782 & _cse784))) + t129);
      _cse689 = ((((_cse690 & 0xfffff800) >> 0xb) | (_cse690 << 0x15)) + _cse780);
      _cse791 = (((_cse682 + _cse784) + ((_cse689 & (~(_cse782))) | (_cse780 & _cse782))) + t130);
      _cse790 = ((((_cse791 & 0xffff0000) >> 0x10) | (_cse791 << 0x10)) + _cse689);
      _cse789 = (((_cse703 + _cse782) + ((_cse790 & (~(_cse780))) | (_cse689 & _cse780))) + t131);
      _cse788 = ((((_cse789 & 0xff800000) >> 0x17) | (_cse789 << 0x9)) + _cse790);
      _cse787 = (((_cse709 + _cse780) + ((_cse788 & (~(_cse689))) | (_cse790 & _cse689))) + t132);
      _cse786 = ((((_cse787 & t127) >> 0x4) | (_cse787 << 0x1c)) + _cse788);
      _cse687 = (((_cse688 + _cse689) + ((_cse786 & (~(_cse790))) | (_cse788 & _cse790))) + t133);
      _cse686 = ((((_cse687 & 0xfffff800) >> 0xb) | (_cse687 << 0x15)) + _cse786);
      _cse797 = (((_cse679 + _cse790) + ((_cse686 & (~(_cse788))) | (_cse786 & _cse788))) + t134);
      _cse796 = ((((_cse797 & 0xffff0000) >> 0x10) | (_cse797 << 0x10)) + _cse686);
      _cse795 = (((_cse706 + _cse788) + ((_cse796 & (~(_cse786))) | (_cse686 & _cse786))) + t135);
      _cse794 = ((((_cse795 & 0xff800000) >> 0x17) | (_cse795 << 0x9)) + _cse796);
      _cse793 = (((_cse712 + _cse786) + ((_cse794 & (~(_cse686))) | (_cse796 & _cse686))) + t136);
      _cse792 = ((((_cse793 & t127) >> 0x4) | (_cse793 << 0x1c)) + _cse794);
      _cse684 = (((_cse685 + _cse686) + ((_cse792 & (~(_cse796))) | (_cse794 & _cse796))) + t137);
      _cse683 = ((((_cse684 & 0xfffff800) >> 0xb) | (_cse684 << 0x15)) + _cse792);
      _cse799 = (((_cse676 + _cse796) + ((_cse683 & (~(_cse794))) | (_cse792 & _cse794))) + t138);
      _cse798 = ((((_cse799 & 0xffff0000) >> 0x10) | (_cse799 << 0x10)) + _cse683);
      _cse803 = (((_cse697 + _cse794) + ((_cse798 & (~(_cse792))) | (_cse683 & _cse792))) + t139);
      _cse802 = ((((_cse803 & 0xff800000) >> 0x17) | (_cse803 << 0x9)) + _cse798);
      _cse801 = (((_cse688 + _cse792) + ((_cse683 | (~(_cse802))) ^ _cse798)) + t140);
      _cse800 = ((((_cse801 & t141) >> 0x6) | (_cse801 << 0x1a)) + _cse802);
      _cse681 = (((_cse682 + _cse683) + ((_cse798 | (~(_cse800))) ^ _cse802)) + t142);
      _cse680 = ((((_cse681 & t143) >> 0xa) | (_cse681 << 0x16)) + _cse800);
      _cse805 = (((_cse700 + _cse798) + ((_cse802 | (~(_cse680))) ^ _cse800)) + t144);
      _cse804 = ((((_cse805 & t145) >> 0xf) | (_cse805 << 0x11)) + _cse680);
      _cse809 = (((_cse715 + _cse802) + ((_cse800 | (~(_cse804))) ^ _cse680)) + t146);
      _cse808 = ((((_cse809 & t147) >> 0x15) | (_cse809 << 0xb)) + _cse804);
      _cse807 = (((_cse685 + _cse800) + ((_cse680 | (~(_cse808))) ^ _cse804)) + t148);
      _cse806 = ((((_cse807 & t141) >> 0x6) | (_cse807 << 0x1a)) + _cse808);
      _cse678 = (((_cse679 + _cse680) + ((_cse804 | (~(_cse806))) ^ _cse808)) + t149);
      _cse677 = ((((_cse678 & t143) >> 0xa) | (_cse678 << 0x16)) + _cse806);
      _cse811 = (((_cse703 + _cse804) + ((_cse808 | (~(_cse677))) ^ _cse806)) + t150);
      _cse810 = ((((_cse811 & t145) >> 0xf) | (_cse811 << 0x11)) + _cse677);
      _cse815 = (((_cse718 + _cse808) + ((_cse806 | (~(_cse810))) ^ _cse677)) + t151);
      _cse814 = ((((_cse815 & t147) >> 0x15) | (_cse815 << 0xb)) + _cse810);
      _cse813 = (((_cse694 + _cse806) + ((_cse677 | (~(_cse814))) ^ _cse810)) + t152);
      _cse812 = ((((_cse813 & t141) >> 0x6) | (_cse813 << 0x1a)) + _cse814);
      _cse675 = (((_cse676 + _cse677) + ((_cse810 | (~(_cse812))) ^ _cse814)) + t153);
      _cse674 = ((((_cse675 & t143) >> 0xa) | (_cse675 << 0x16)) + _cse812);
      _cse817 = (((_cse706 + _cse810) + ((_cse814 | (~(_cse674))) ^ _cse812)) + t154);
      _cse816 = ((((_cse817 & t145) >> 0xf) | (_cse817 << 0x11)) + _cse674);
      _cse822 = (((_cse709 + _cse814) + ((_cse812 | (~(_cse816))) ^ _cse674)) + t155);
      _cse821 = (((_cse822 & t147) >> 0x15) | (_cse822 << 0xb));
      _cse820 = (_cse821 + _cse816);
      _cse819 = (((_cse691 + _cse812) + ((_cse674 | (~(_cse820))) ^ _cse816)) + t156);
      _cse818 = ((((_cse819 & t141) >> 0x6) | (_cse819 << 0x1a)) + _cse820);
      _cse672 = (((_cse673 + _cse674) + ((_cse816 | (~(_cse818))) ^ _cse820)) + t157);
      _cse671 = (((_cse672 & t143) >> 0xa) | (_cse672 << 0x16));
      _cse670 = (_cse671 + _cse818);
      _cse669 = (_cse670 + _cse722);
      (*(uint32_t*)(t104)) = ((uint32_t)(_cse669));
      t158 = var_1ce8;
      _cse824 = (((_cse697 + _cse816) + ((_cse820 | (~(_cse670))) ^ _cse818)) + t158);
      _cse823 = ((((_cse824 & t145) >> 0xf) | (_cse824 << 0x11)) + _cse670);
      _cse825 = (((_cse712 + _cse820) + ((_cse818 | (~(_cse823))) ^ _cse670)) + var_1cf0);
      (*var_718) = ((uint32_t)(((_cse823 + _cse725) + (((_cse825 & t147) >> 0x15) | (_cse825 << 0xb)))));
      (*var_710) = ((uint32_t)((_cse823 + _cse724)));
      (*var_708) = ((uint32_t)((_cse818 + _cse719)));
      /* 1 argument slot(s) hold values this function never wrote: either the callee reads a stale register or a definition was lost here */
      t159 = vm_memset(var_738, 0x0, 0x38, 0x10016a2c, _cse820, _cse670, _cse669, t104);
      var_1364 = 0x263afc19;
      t0 = t1 = t2 = t3 = t4 = 0x0; /* t0, t1, t2, t3, t4: never set up on this edge */
    }
    goto L_0x10004d40;
L_0x10007bcc:
    // b182 @0x10007bcc
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x28f426de) == 0x1)) {
      // b308 @0x10011140
      t639 = var_fe0;
      t640 = var_12c8;
      (*(uint64_t*)(t639)) = var_12d0;
      (*(uint64_t*)((t639 + 0x8))) = t640;
      t641 = var_fe0;
      t642 = var_12c0;
      (*(uint64_t*)((t639 + 0x18))) = var_12b8;
      (*(uint64_t*)((t639 + 0x10))) = t642;
      t643 = var_fe0;
      t644 = var_12b0;
      t645 = var_12a8;
      (*(uint64_t*)((t641 + 0x28))) = t645;
      (*(uint64_t*)((t641 + 0x20))) = t644;
      t646 = var_12a0;
      (*(uint64_t*)((t643 + 0x38))) = var_1298;
      (*(uint64_t*)((t643 + 0x30))) = t646;
      var_1364 = 0x169b2590;
      t0 = t645;
      goto L_0x10004d40;
    }
    // b184 @0x10007be8
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x2d207b9c) == 0x1)) {
      // b186 @0x10007c04
      t189 = var_19c0;
      t190 = var_1aa8;
      _cse826 = (__entry_r125 - 0x1180);
      t191 = (*(uint32_t*)((_cse826 + (((t189 + 0xfffffffe) & 0xffffffff) << 0x2))));
      t192 = (*(uint32_t*)((_cse826 + (((t189 + var_1b18) & 0xffffffff) << 0x2))));
      t193 = (*(uint32_t*)((_cse826 + (((t189 + 0xfffffff9) & 0xffffffff) << 0x2))));
      t194 = (*(uint32_t*)((_cse826 + (((t189 + var_1ad8) & 0xffffffff) << 0x2))));
      _cse827 = (((int64_t)(int32_t)(t191)) & 0xffffffff);
      _cse828 = (((int64_t)(int32_t)(t192)) & 0xffffffff);
      (*(uint32_t*)((_cse826 + (t189 << 0x2)))) = ((uint32_t)((((((int64_t)(int32_t)(t194)) + ((int64_t)(int32_t)(t193))) + ((((_cse827 >> 0x5) | (_cse827 << 0x1b)) ^ (_cse827 >> 0x2)) ^ ((_cse827 >> 0x9) | (_cse827 << 0x17)))) + ((((_cse828 >> 0xb) | (_cse828 << 0x15)) ^ (_cse828 >> 0x5)) ^ ((_cse828 >> 0x3) | (_cse828 << 0x1d))))));
      var_19c0 = (t189 + 0x1);
      if (((var_19c0 == t190) == 0x1)) {
        var_1364 = 0x1ec32ac4;
      } else {
        var_1364 = 0x2d207b9c;
      }
      _cse830 = ((int64_t)(int32_t)(t194));
      _cse831 = ((int64_t)(int32_t)(t193));
      _cse829 = (_cse830 + _cse831);
      _cse832 = (((int64_t)(int32_t)(t191)) & 0xffffffff);
      _cse835 = (((int64_t)(int32_t)(t192)) & 0xffffffff);
      _cse834 = (((_cse835 >> 0xb) | (_cse835 << 0x15)) ^ (_cse835 >> 0x5));
      _cse836 = ((_cse835 >> 0x3) | (_cse835 << 0x1d));
      _cse833 = (_cse834 ^ _cse836);
      t0 = ((_cse829 + ((((_cse832 >> 0x5) | (_cse832 << 0x1b)) ^ (_cse832 >> 0x2)) ^ ((_cse832 >> 0x9) | (_cse832 << 0x17)))) + _cse833);
      t1 = _cse829;
      t2 = _cse833;
      t3 = _cse830;
      t4 = (((t189 + 0xfffffff9) & 0xffffffff) << 0x2);
    }
    goto L_0x10004d40;
L_0x10007214:
    // b114 @0x10007214
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x2f8e4f4d) == 0x1)) {
      // b292 @0x1000ca70
      t436 = var_1370;
      t437 = var_fd0;
      _cse837 = (var_1b30 + t436);
      t438 = (*(uint8_t*)(_cse837));
      t439 = (*(uint8_t*)((_cse837 + 0x1)));
      t440 = (*(uint8_t*)((_cse837 + 0x2)));
      t441 = (*(uint8_t*)((_cse837 + 0x3)));
      t442 = (*(uint8_t*)((_cse837 + 0x4)));
      t443 = (*(uint8_t*)((_cse837 + 0x5)));
      t444 = (*(uint8_t*)((_cse837 + 0x6)));
      t445 = (*(uint8_t*)((_cse837 + 0x7)));
      t446 = (*(uint8_t*)((_cse837 + 0x8)));
      t447 = (*(uint8_t*)((_cse837 + 0x9)));
      t448 = (*(uint8_t*)((_cse837 + 0xa)));
      t449 = (*(uint8_t*)((_cse837 + 0xb)));
      t450 = (*(uint8_t*)((_cse837 + 0xc)));
      t451 = (*(uint8_t*)((_cse837 + 0xd)));
      t452 = (*(uint8_t*)((_cse837 + 0xe)));
      t453 = (*(uint8_t*)((_cse837 + 0xf)));
      t454 = (*(uint8_t*)((_cse837 + 0x10)));
      t455 = (*(uint8_t*)((_cse837 + 0x11)));
      t456 = (*(uint8_t*)((_cse837 + 0x12)));
      t457 = (*(uint8_t*)((_cse837 + 0x13)));
      t458 = (*(uint8_t*)((_cse837 + 0x14)));
      t459 = (*(uint8_t*)((_cse837 + 0x15)));
      t460 = (*(uint8_t*)((_cse837 + 0x16)));
      t461 = (*(uint8_t*)((_cse837 + 0x17)));
      t462 = (*(uint8_t*)((_cse837 + 0x18)));
      t463 = (*(uint8_t*)((_cse837 + 0x19)));
      t464 = (*(uint8_t*)((_cse837 + 0x1a)));
      t465 = (*(uint8_t*)((_cse837 + 0x1b)));
      t466 = (*(uint8_t*)((_cse837 + 0x1c)));
      t467 = (*(uint8_t*)((_cse837 + 0x1d)));
      t468 = (*(uint8_t*)((_cse837 + 0x1e)));
      _cse838 = (t437 + t436);
      (*(uint8_t*)((_cse838 + 0x1f))) = (*(uint8_t*)((_cse837 + 0x1f)));
      (*(uint8_t*)((_cse838 + 0x1e))) = t468;
      (*(uint8_t*)((_cse838 + 0x1d))) = t467;
      (*(uint8_t*)((_cse838 + 0x1c))) = t466;
      (*(uint8_t*)((_cse838 + 0x1b))) = t465;
      (*(uint8_t*)((_cse838 + 0x1a))) = t464;
      (*(uint8_t*)((_cse838 + 0x19))) = t463;
      (*(uint8_t*)((_cse838 + 0x18))) = t462;
      (*(uint8_t*)((_cse838 + 0x17))) = t461;
      (*(uint8_t*)((_cse838 + 0x16))) = t460;
      (*(uint8_t*)((_cse838 + 0x15))) = t459;
      (*(uint8_t*)((_cse838 + 0x14))) = t458;
      (*(uint8_t*)((_cse838 + 0x13))) = t457;
      (*(uint8_t*)((_cse838 + 0x12))) = t456;
      (*(uint8_t*)((_cse838 + 0x11))) = t455;
      (*(uint8_t*)((_cse838 + 0x10))) = t454;
      (*(uint8_t*)((_cse838 + 0xf))) = t453;
      (*(uint8_t*)((_cse838 + 0xe))) = t452;
      (*(uint8_t*)((_cse838 + 0xd))) = t451;
      (*(uint8_t*)((_cse838 + 0xc))) = t450;
      (*(uint8_t*)((_cse838 + 0xb))) = t449;
      (*(uint8_t*)((_cse838 + 0xa))) = t448;
      (*(uint8_t*)((_cse838 + 0x9))) = t447;
      (*(uint8_t*)((_cse838 + 0x8))) = t446;
      (*(uint8_t*)((_cse838 + 0x7))) = t445;
      (*(uint8_t*)((_cse838 + 0x6))) = t444;
      (*(uint8_t*)((_cse838 + 0x5))) = t443;
      (*(uint8_t*)((_cse838 + 0x4))) = t442;
      (*(uint8_t*)((_cse838 + 0x3))) = t441;
      (*(uint8_t*)((_cse838 + 0x2))) = t440;
      (*(uint8_t*)((_cse838 + 0x1))) = t439;
      (*(uint8_t*)(_cse838)) = t438;
      var_1370 = (t436 + 0x20);
      if (((var_1370 == var_fc8) == 0x1)) {
        var_1364 = 0xfffffffffb03d450;
      } else {
        var_1364 = 0x2f8e4f4d;
      }
      t0 = 0xfffffffffb03d450;
      t1 = ((int64_t)(int8_t)(t438));
      t2 = ((int64_t)(int8_t)(t439));
      t3 = ((int64_t)(int8_t)(t440));
      t4 = ((int64_t)(int8_t)(t441));
      goto L_0x10004d40;
    }
    // b116 @0x10007230
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x3083817c) == 0x1)) {
      // b118 @0x1000724c
      t166 = var_1a38;
      var_3cc = var_1a54;
      t167 = var_1a50;
      var_3c8 = t167;
      t168 = var_1a4c;
      var_3c4 = t168;
      t169 = var_1a5c;
      var_3d0 = t169;
      t170 = var_1a40;
      var_3c0 = t170;
      t171 = var_1a3c;
      var_3bc = t171;
      t172 = var_1a48;
      _cse839 = (t166 << 0x2);
      t173 = (*(uint32_t*)(((__entry_r125 - 0x1180) + _cse839)));
      _cse840 = ((int64_t)(int32_t)(var_1a44));
      t174 = (*(uint32_t*)(((((((_cse840 & 0xff) % 0x3) == var_1a98) == 0x1) ? 0x100002d0 : 0x100003d0) + _cse839)));
      var_748 = ((uint32_t)(_cse841));
      var_744 = ((uint32_t)(_cse844));
      _cse843 = ((int64_t)(int32_t)(var_1a54));
      _cse842 = ((((((((int64_t)(int32_t)(t168)) & (~(_cse843))) | (_cse843 & ((int64_t)(int32_t)(t167)))) + ((int64_t)(int32_t)(t172))) + ((int64_t)(int32_t)(t173))) + (((((_cse843 & var_1ac8) >> 0x7) | (_cse843 << 0x19)) ^ (((_cse843 & 0xffffe000) >> 0xd) | (_cse843 << 0x13))) ^ (((_cse843 & 0xfffe0000) >> 0x11) | (_cse843 << 0xf)))) + ((int64_t)(int32_t)(t174)));
      _cse841 = (_cse842 + ((int64_t)(int32_t)(var_1a58)));
      _cse845 = ((int64_t)(int32_t)(t169));
      _cse846 = ((int64_t)(int32_t)(t170));
      _cse847 = ((int64_t)(int32_t)(t171));
      _cse844 = (((((_cse845 ^ _cse846) & _cse847) ^ (_cse845 & _cse846)) + (((((_cse847 & 0xffffffe0) >> 0x5) | (_cse847 << 0x1b)) ^ (((_cse847 & 0xfffff800) >> 0xb) | (_cse847 << 0x15))) ^ (((_cse847 & 0xfff80000) >> 0x13) | (_cse847 << 0xd)))) + _cse842);
      _cse848 = (t166 + 0x1);
      if (((_cse848 == var_1aa8) == 0x1)) {
        var_1364 = 0xffffffffd170b27a;
      } else {
        var_1364 = 0x3083817c;
      }
      var_1a38 = _cse848;
      var_1a3c = var_744;
      var_1a40 = t171;
      var_1a44 = ((uint32_t)(((_cse840 & 0xffffffff) + 0x1)));
      var_1a48 = t168;
      var_1a4c = t167;
      var_1a50 = var_1a54;
      var_1a54 = var_748;
      var_1a58 = t169;
      var_1a5c = t170;
      _cse851 = ((int64_t)(int32_t)(t169));
      _cse852 = ((int64_t)(int32_t)(t170));
      _cse853 = ((int64_t)(int32_t)(t171));
      _cse850 = ((_cse851 ^ _cse852) & _cse853);
      _cse854 = (_cse851 & _cse852);
      _cse849 = (_cse850 ^ _cse854);
      t0 = (_cse849 + (((((_cse853 & 0xffffffe0) >> 0x5) | (_cse853 << 0x1b)) ^ (((_cse853 & 0xfffff800) >> 0xb) | (_cse853 << 0x15))) ^ (((_cse853 & 0xfff80000) >> 0x13) | (_cse853 << 0xd))));
      t1 = (t166 + 0x1);
      t2 = _cse849;
      t3 = _cse854;
      t4 = _cse851;
    }
    goto L_0x10004d40;
L_0x10009574:
    // b238 @0x10009574
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x3730d54b) == 0x1)) {
      // b324 @0x10015254
      t875 = var_1400;
      var_4e0 = var_141c;
      t876 = var_1418;
      var_4dc = t876;
      t877 = var_1414;
      var_4d8 = t877;
      t878 = var_1424;
      var_4e4 = t878;
      t879 = var_1408;
      var_4d4 = t879;
      t880 = var_1404;
      var_4d0 = t880;
      t881 = var_1410;
      _cse855 = (t875 << 0x2);
      t882 = (*(uint32_t*)(((__entry_r125 - 0x1180) + _cse855)));
      _cse856 = ((int64_t)(int32_t)(var_140c));
      t883 = (*(uint32_t*)(((((((_cse856 & 0xff) % 0x3) == var_1a98) == 0x1) ? 0x100002d0 : 0x100003d0) + _cse855)));
      var_c6c = ((uint32_t)(_cse857));
      var_c68 = ((uint32_t)(_cse860));
      _cse859 = ((int64_t)(int32_t)(var_141c));
      _cse858 = ((((((((int64_t)(int32_t)(t877)) & (~(_cse859))) | (_cse859 & ((int64_t)(int32_t)(t876)))) + ((int64_t)(int32_t)(t881))) + ((int64_t)(int32_t)(t882))) + (((((_cse859 & var_1ac8) >> 0x7) | (_cse859 << 0x19)) ^ (((_cse859 & 0xffffe000) >> 0xd) | (_cse859 << 0x13))) ^ (((_cse859 & 0xfffe0000) >> 0x11) | (_cse859 << 0xf)))) + ((int64_t)(int32_t)(t883)));
      _cse857 = (_cse858 + ((int64_t)(int32_t)(var_1420)));
      _cse861 = ((int64_t)(int32_t)(t878));
      _cse862 = ((int64_t)(int32_t)(t879));
      _cse863 = ((int64_t)(int32_t)(t880));
      _cse860 = (((((_cse861 ^ _cse862) & _cse863) ^ (_cse861 & _cse862)) + (((((_cse863 & 0xffffffe0) >> 0x5) | (_cse863 << 0x1b)) ^ (((_cse863 & 0xfffff800) >> 0xb) | (_cse863 << 0x15))) ^ (((_cse863 & 0xfff80000) >> 0x13) | (_cse863 << 0xd)))) + _cse858);
      _cse864 = (t875 + 0x1);
      if (((_cse864 == var_1aa8) == 0x1)) {
        var_1364 = 0xffffffffa83e330a;
      } else {
        var_1364 = 0x3730d54b;
      }
      var_1400 = _cse864;
      var_1404 = var_c68;
      var_1408 = t880;
      var_140c = ((uint32_t)(((_cse856 & 0xffffffff) + 0x1)));
      var_1410 = t877;
      var_1414 = t876;
      var_1418 = var_141c;
      var_141c = var_c6c;
      var_1420 = t878;
      var_1424 = t879;
      _cse867 = ((int64_t)(int32_t)(t878));
      _cse868 = ((int64_t)(int32_t)(t879));
      _cse869 = ((int64_t)(int32_t)(t880));
      _cse866 = ((_cse867 ^ _cse868) & _cse869);
      _cse870 = (_cse867 & _cse868);
      _cse865 = (_cse866 ^ _cse870);
      t0 = (_cse865 + (((((_cse869 & 0xffffffe0) >> 0x5) | (_cse869 << 0x1b)) ^ (((_cse869 & 0xfffff800) >> 0xb) | (_cse869 << 0x15))) ^ (((_cse869 & 0xfff80000) >> 0x13) | (_cse869 << 0xd))));
      t1 = (t875 + 0x1);
      t2 = _cse865;
      t3 = _cse870;
      t4 = _cse867;
      goto L_0x10004d40;
    }
    // b240 @0x10009590
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x39a41b0f) == 0x1)) {
      // b242 @0x100095ac
      t283 = (*var_f50);
      var_c8c = t283;
      t284 = (*var_f48);
      var_c88 = t284;
      t285 = (*var_f40);
      var_c84 = t285;
      t286 = (*var_f38);
      var_c80 = t286;
      t287 = (*var_f30);
      var_c7c = t287;
      t288 = (*var_f28);
      var_c78 = t288;
      t289 = (*var_f20);
      var_c74 = t289;
      t290 = (*var_f18);
      var_c70 = t290;
      var_1364 = 0x3730d54b;
      var_1400 = var_1a98;
      var_140c = ((uint32_t)(var_1a98));
      var_1404 = t283;
      var_1408 = t284;
      var_1420 = t286;
      var_141c = t287;
      var_1418 = t288;
      var_1414 = t289;
      var_1410 = t290;
      var_1424 = t285;
    }
    goto L_0x10004d40;
L_0x100066ec:
    // b50 @0x100066ec
    if ((((int64_t)(0x4433ffee) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b52 @0x10006708
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1ac0) != 0x1)) {
        // b54 @0x10006720
        if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x4433ffef) == 0x1)) {
          // b56 @0x1000673c
          if ((((((int64_t)(int32_t)(var_79c)) & 0xffffffff) < var_1aa8) == 0x1)) {
            var_1364 = 0xffffffff8818e777;
          } else {
            var_1364 = 0xffffffffc0a982cf;
          }
          goto L_0x10004d40;
        } else {
          goto L_0x10004d40;
        }
      }
      // b332 @0x10016334
      return ((int64_t)(int32_t)(var_1a7c));
    }
    // b194 @0x10007e48
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x3ba517f7) == 0x1)) {
      // b311 @0x10011bbc
      _cse871 = (var_fe0 + 0x40);
      var_fd0 = (uint8_t*)_cse871;
      if (((_cse871 < var_1b30) == 0x1)) {
        var_1364 = 0xffffffffbb17d68b;
      } else {
        var_1364 = 0xffffffff852eddc3;
      }
      goto L_0x10004d40;
    }
    // b196 @0x10007e64
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x3ce20c75) == 0x1)) {
      // b312 @0x10011c28
      t671 = var_16c8;
      var_4c4 = var_16e4;
      t672 = var_16e0;
      var_4c0 = t672;
      t673 = var_16dc;
      var_4bc = t673;
      t674 = var_16ec;
      var_4c8 = t674;
      t675 = var_16d0;
      var_4b8 = t675;
      t676 = var_16cc;
      var_4b4 = t676;
      t677 = var_16d8;
      _cse872 = (t671 << 0x2);
      t678 = (*(uint32_t*)(((__entry_r125 - 0x1180) + _cse872)));
      _cse873 = ((int64_t)(int32_t)(var_16d4));
      t679 = (*(uint32_t*)(((((((_cse873 & 0xff) % 0x3) == var_1a98) == 0x1) ? 0x100002d0 : 0x100003d0) + _cse872)));
      var_c34 = ((uint32_t)(_cse874));
      var_c30 = ((uint32_t)(_cse877));
      _cse876 = ((int64_t)(int32_t)(var_16e4));
      _cse875 = ((((((((int64_t)(int32_t)(t673)) & (~(_cse876))) | (_cse876 & ((int64_t)(int32_t)(t672)))) + ((int64_t)(int32_t)(t677))) + ((int64_t)(int32_t)(t678))) + (((((_cse876 & var_1ac8) >> 0x7) | (_cse876 << 0x19)) ^ (((_cse876 & 0xffffe000) >> 0xd) | (_cse876 << 0x13))) ^ (((_cse876 & 0xfffe0000) >> 0x11) | (_cse876 << 0xf)))) + ((int64_t)(int32_t)(t679)));
      _cse874 = (_cse875 + ((int64_t)(int32_t)(var_16e8)));
      _cse878 = ((int64_t)(int32_t)(t674));
      _cse879 = ((int64_t)(int32_t)(t675));
      _cse880 = ((int64_t)(int32_t)(t676));
      _cse877 = (((((_cse878 ^ _cse879) & _cse880) ^ (_cse878 & _cse879)) + (((((_cse880 & 0xffffffe0) >> 0x5) | (_cse880 << 0x1b)) ^ (((_cse880 & 0xfffff800) >> 0xb) | (_cse880 << 0x15))) ^ (((_cse880 & 0xfff80000) >> 0x13) | (_cse880 << 0xd)))) + _cse875);
      _cse881 = (t671 + 0x1);
      if (((_cse881 == var_1aa8) == 0x1)) {
        var_1364 = 0xffffffffa8246971;
      } else {
        var_1364 = 0x3ce20c75;
      }
      var_16c8 = _cse881;
      var_16cc = var_c30;
      var_16d0 = t676;
      var_16d4 = ((uint32_t)(((_cse873 & 0xffffffff) + 0x1)));
      var_16d8 = t673;
      var_16dc = t672;
      var_16e0 = var_16e4;
      var_16e4 = var_c34;
      var_16e8 = t674;
      var_16ec = t675;
      _cse884 = ((int64_t)(int32_t)(t674));
      _cse885 = ((int64_t)(int32_t)(t675));
      _cse886 = ((int64_t)(int32_t)(t676));
      _cse883 = ((_cse884 ^ _cse885) & _cse886);
      _cse887 = (_cse884 & _cse885);
      _cse882 = (_cse883 ^ _cse887);
      t0 = (_cse882 + (((((_cse886 & 0xffffffe0) >> 0x5) | (_cse886 << 0x1b)) ^ (((_cse886 & 0xfffff800) >> 0xb) | (_cse886 << 0x15))) ^ (((_cse886 & 0xfff80000) >> 0x13) | (_cse886 << 0xd))));
      t1 = (t671 + 0x1);
      t2 = _cse882;
      t3 = _cse887;
      t4 = _cse884;
      goto L_0x10004d40;
    }
    // b198 @0x10007e80
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x3dc7793b) == 0x1)) {
      // b200 @0x10007e9c
      t196 = var_1a00;
      var_3e4 = var_1a1c;
      t197 = var_1a18;
      var_3e0 = t197;
      t198 = var_1a14;
      var_3dc = t198;
      t199 = var_1a24;
      var_3e8 = t199;
      t200 = var_1a08;
      var_3d8 = t200;
      t201 = var_1a04;
      var_3d4 = t201;
      t202 = var_1a10;
      _cse888 = (t196 << 0x2);
      t203 = (*(uint32_t*)(((__entry_r125 - 0x1180) + _cse888)));
      _cse889 = ((int64_t)(int32_t)(var_1a0c));
      t204 = (*(uint32_t*)(((((((_cse889 & 0xff) % 0x3) == var_1a98) == 0x1) ? 0x100002d0 : 0x100003d0) + _cse888)));
      var_770 = ((uint32_t)(_cse890));
      var_76c = ((uint32_t)(_cse893));
      _cse892 = ((int64_t)(int32_t)(var_1a1c));
      _cse891 = ((((((((int64_t)(int32_t)(t198)) & (~(_cse892))) | (_cse892 & ((int64_t)(int32_t)(t197)))) + ((int64_t)(int32_t)(t202))) + ((int64_t)(int32_t)(t203))) + (((((_cse892 & var_1ac8) >> 0x7) | (_cse892 << 0x19)) ^ (((_cse892 & 0xffffe000) >> 0xd) | (_cse892 << 0x13))) ^ (((_cse892 & 0xfffe0000) >> 0x11) | (_cse892 << 0xf)))) + ((int64_t)(int32_t)(t204)));
      _cse890 = (_cse891 + ((int64_t)(int32_t)(var_1a20)));
      _cse894 = ((int64_t)(int32_t)(t199));
      _cse895 = ((int64_t)(int32_t)(t200));
      _cse896 = ((int64_t)(int32_t)(t201));
      _cse893 = (((((_cse894 ^ _cse895) & _cse896) ^ (_cse894 & _cse895)) + (((((_cse896 & 0xffffffe0) >> 0x5) | (_cse896 << 0x1b)) ^ (((_cse896 & 0xfffff800) >> 0xb) | (_cse896 << 0x15))) ^ (((_cse896 & 0xfff80000) >> 0x13) | (_cse896 << 0xd)))) + _cse891);
      _cse897 = (t196 + 0x1);
      if (((_cse897 == var_1aa8) == 0x1)) {
        var_1364 = 0x5b5b41f7;
      } else {
        var_1364 = 0x3dc7793b;
      }
      var_1a00 = _cse897;
      var_1a04 = var_76c;
      var_1a08 = t201;
      var_1a0c = ((uint32_t)(((_cse889 & 0xffffffff) + 0x1)));
      var_1a10 = t198;
      var_1a14 = t197;
      var_1a18 = var_1a1c;
      var_1a1c = var_770;
      var_1a20 = t199;
      var_1a24 = t200;
      _cse900 = ((int64_t)(int32_t)(t199));
      _cse901 = ((int64_t)(int32_t)(t200));
      _cse902 = ((int64_t)(int32_t)(t201));
      _cse899 = ((_cse900 ^ _cse901) & _cse902);
      _cse903 = (_cse900 & _cse901);
      _cse898 = (_cse899 ^ _cse903);
      t0 = (_cse898 + (((((_cse902 & 0xffffffe0) >> 0x5) | (_cse902 << 0x1b)) ^ (((_cse902 & 0xfffff800) >> 0xb) | (_cse902 << 0x15))) ^ (((_cse902 & 0xfff80000) >> 0x13) | (_cse902 << 0xd))));
      t1 = (t196 + 0x1);
      t2 = _cse898;
      t3 = _cse903;
      t4 = _cse900;
    }
    goto L_0x10004d40;
L_0x10009870:
    // b252 @0x10009870
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1b48) == 0x1)) {
      // b254 @0x10009888
      var_1010 = (uint8_t*)&var_1360;
      t291 = vm_memset(&var_1360, 0x0, 0x20, t0, t1, t2, t3, t4);
      var_1008 = t7;
      var_1000 = (__entry_r125 - 0x1290);
      t292 = vm_memset(&var_1290, 0x0, 0x40);
      if ((((uint64_t)(&var_1290) < 0x10000290) == 0x1)) {
        var_1364 = 0xffffffffa253bf54;
      } else {
        var_1364 = 0xffffffffa7375b0e;
      }
      t0 = t1 = t2 = t3 = t4 = 0x0; /* t0, t1, t2, t3, t4: never set up on this edge */
    }
    goto L_0x10004d40;
L_0x10007624:
    // b132 @0x10007624
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x5384b4e3) == 0x1)) {
      // b296 @0x1000cd7c
      t470 = var_4e8;
      t471 = var_f70;
      var_4cc = var_1428;
      _cse904 = (((int64_t)(int32_t)(t470)) + 0x1);
      var_13ec = ((uint32_t)(_cse904));
      var_13dc = var_1428;
      var_13e8 = (_cse904 & 0xffffffff);
      if (((var_13e8 < t471) == 0x1)) {
        var_1364 = 0xffffffffa2df69c5;
      } else {
        var_1364 = 0x323eeb55;
      }
      if (((((((int64_t)(int32_t)(t470)) + 0x1) & 0xffffffff) < t471) == 0x1)) {
        t0 = 0xffffffffa2df69c5;
      } else {
        t0 = 0x323eeb55;
      }
      t1 = 0xffffffffa2df69c5;
      goto L_0x10004d40;
    }
    // b134 @0x10007640
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x566b9a51) == 0x1)) {
      // b136 @0x1000765c
      if ((((((int64_t)(int32_t)(var_c64)) & 0xffffffff) < var_1aa8) == 0x1)) {
        var_1364 = 0xffffffffedd2a4cd;
      } else {
        var_1364 = 0xffffffffe0bbcd6f;
      }
    }
    goto L_0x10004d40;
L_0x10007a14:
    // b164 @0x10007a14
    if ((((int64_t)(0x5e619a9a) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b166 @0x10007a30
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x5e619a9b) != 0x1)) {
        goto L_0x10007a4c;
      }
      // b303 @0x1000ec60
      t505 = var_1008;
      t506 = var_135e;
      t507 = var_135d;
      t508 = var_135c;
      t509 = var_135b;
      t510 = (*(uint8_t*)(&var_135a));
      t511 = var_1359;
      t512 = (*(uint8_t*)(&var_1358));
      t513 = var_1357;
      t514 = (*(uint8_t*)(&var_1356));
      t515 = var_1355;
      t516 = var_1352;
      t517 = var_1353;
      t518 = (*(uint8_t*)(&var_1354));
      _cse905 = (((int64_t)(int8_t)(var_135f)) & 0xff);
      _cse906 = (((int64_t)(int8_t)(t506)) & 0xff);
      _cse907 = (((int64_t)(int8_t)(t507)) & 0xff);
      _cse908 = (((int64_t)(int8_t)(t508)) & 0xff);
      _cse909 = (((int64_t)(int8_t)(t509)) & 0xff);
      _cse910 = (((int64_t)(int8_t)(t510)) & 0xff);
      _cse911 = (((int64_t)(int8_t)(t511)) & 0xff);
      _cse912 = (((int64_t)(int8_t)(t512)) & 0xff);
      _cse913 = (((int64_t)(int8_t)(t513)) & 0xff);
      _cse914 = (((int64_t)(int8_t)(t514)) & 0xff);
      _cse915 = (((int64_t)(int8_t)(t515)) & 0xff);
      _cse916 = (((int64_t)(int8_t)(t518)) & 0xff);
      _cse917 = (((int64_t)(int8_t)(t517)) & 0xff);
      _cse918 = (((int64_t)(int8_t)(t516)) & 0xff);
      (*var_1d10) = ((uint32_t)(((((((((((((((((((((((((((((((((((((int64_t)(int8_t)((*var_1010))) * 0x85) + _cse905) & 0xff) * 0x85) + _cse906) * 0x85) + _cse907) * 0x85) + _cse908) & 0xff) * 0x85) + _cse909) * 0x85) + _cse910) * 0x85) + _cse911) & 0xff) * 0x85) + _cse912) * 0x85) + _cse913) * 0x85) + _cse914) & 0xff) * 0x85) + _cse915) * 0x85) + _cse916) * 0x85) + _cse917) & 0x3f) | ((((((((((((((((((((((((((((((((_cse905 * 0x85) + _cse906) * 0x85) + _cse907) & 0xff) * 0x85) + _cse908) * 0x85) + _cse909) * 0x85) + _cse910) & 0xff) * 0x85) + _cse911) * 0x85) + _cse912) * 0x85) + _cse913) & 0xff) * 0x85) + _cse914) * 0x85) + _cse915) * 0x85) + _cse916) & 0xff) * 0x85) + _cse917) * 0x85) + _cse918) << 0x8) & 0x6f00)) | ((((((((((((((((((((((((((((((((_cse906 * 0x85) + _cse907) * 0x85) + _cse908) & 0xff) * 0x85) + _cse909) * 0x85) + _cse910) * 0x85) + _cse911) & 0xff) * 0x85) + _cse912) * 0x85) + _cse913) * 0x85) + _cse914) & 0xff) * 0x85) + _cse915) * 0x85) + _cse916) * 0x85) + _cse917) & 0xff) * 0x85) + _cse918) * 0x85) + ((int64_t)(int8_t)((*var_1a78)))) << 0x10) & 0x7b0000)) | 0x44000000)));
      (*var_1d08) = var_1d00;
      var_1364 = ((uint32_t)(var_1ac0));
      var_1a7c = ((uint32_t)(var_1a98));
      _cse920 = ((int64_t)(int8_t)(t518));
      _cse919 = (_cse920 & 0xff);
      _cse922 = ((int64_t)(int8_t)(t517));
      _cse921 = (_cse922 & 0xff);
      t0 = ((((((((((((((((((((((((((((int64_t)(int8_t)(t506)) & 0xff) * 0x85) + (((int64_t)(int8_t)(t507)) & 0xff)) * 0x85) + (((int64_t)(int8_t)(t508)) & 0xff)) & 0xff) * 0x85) + (((int64_t)(int8_t)(t509)) & 0xff)) * 0x85) + (((int64_t)(int8_t)(t510)) & 0xff)) * 0x85) + (((int64_t)(int8_t)(t511)) & 0xff)) & 0xff) * 0x85) + (((int64_t)(int8_t)(t512)) & 0xff)) * 0x85) + (((int64_t)(int8_t)(t513)) & 0xff)) * 0x85) + (((int64_t)(int8_t)(t514)) & 0xff)) & 0xff) * 0x85) + (((int64_t)(int8_t)(t515)) & 0xff)) * 0x85) + _cse919) * 0x85) + _cse921);
      t1 = (((int64_t)(int8_t)(t516)) & 0xff);
      t2 = _cse921;
      t3 = _cse919;
      t4 = 0x6f00;
      t7 = t505;
      goto L_0x10004d40;
    }
    // b270 @0x10009bfc
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x5b5b41f7) == 0x1)) {
      // b331 @0x10016210
      (*var_a00) = ((uint32_t)((((int64_t)(int32_t)(var_3dc)) + ((int64_t)(int32_t)(var_790)))));
      (*var_9f8) = ((uint32_t)((((int64_t)(int32_t)(var_3e0)) + ((int64_t)(int32_t)(var_78c)))));
      (*var_9f0) = ((uint32_t)((((int64_t)(int32_t)(var_3e4)) + ((int64_t)(int32_t)(var_788)))));
      (*var_9e8) = ((uint32_t)((((int64_t)(int32_t)(var_770)) + ((int64_t)(int32_t)(var_784)))));
      (*var_9e0) = ((uint32_t)((((int64_t)(int32_t)(var_3e8)) + ((int64_t)(int32_t)(var_780)))));
      (*var_9d8) = ((uint32_t)((((int64_t)(int32_t)(var_3d8)) + ((int64_t)(int32_t)(var_77c)))));
      (*var_9d0) = ((uint32_t)((((int64_t)(int32_t)(var_3d4)) + ((int64_t)(int32_t)(var_778)))));
      (*var_9c8) = ((uint32_t)((((int64_t)(int32_t)(var_76c)) + ((int64_t)(int32_t)(var_774)))));
      /* 1 argument slot(s) hold values this function never wrote: either the callee reads a stale register or a definition was lost here */
      t923 = vm_memset(var_a18, 0x0, 0x38, 0x10016a2c, t1, t2, t3, t4);
      var_1364 = ((uint32_t)(var_1ae8));
      t0 = t1 = t2 = t3 = t4 = 0x0; /* t0, t1, t2, t3, t4: never set up on this edge */
      goto L_0x10004d40;
    }
    // b272 @0x10009c18
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x5c2011df) == 0x1)) {
      // b274 @0x10009c34
      var_f90 = (var_1ad0 & 0x1f);
      var_1364 = 0xbc64ede2;
      var_13a8 = var_1a98;
      var_f88 = (var_1ad0 - var_f90);
      _cse923 = (var_f90 - var_1ad0);
      var_f80 = (var_fa0 + _cse923);
      var_f78 = (var_f98 + _cse923);
    }
    goto L_0x10004d40;
L_0x10007a4c:
    // b168 @0x10007a4c
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x6326e7c8) == 0x1)) {
      // b304 @0x1000efc4
      (*var_f68) = 0xfc;
      var_1430 = (__entry_r125 - 0x1144);
      var_1364 = 0xe0dad150;
      var_1438 = (__entry_r125 - 0x1148);
      var_1440 = (__entry_r125 - 0x114c);
      var_1448 = (__entry_r125 - 0x104b);
      var_1450 = (__entry_r125 - 0x104a);
      var_1458 = (__entry_r125 - 0x1049);
      var_1460 = (__entry_r125 - 0x104c);
      var_1468 = (__entry_r125 - 0x1150);
      var_1470 = (__entry_r125 - 0x104f);
      var_1478 = (__entry_r125 - 0x104e);
      var_1480 = (__entry_r125 - 0x104d);
      var_1488 = (__entry_r125 - 0x1050);
      var_1490 = (__entry_r125 - 0x1154);
      var_1498 = (__entry_r125 - 0x1053);
      var_14a0 = (__entry_r125 - 0x1052);
      var_14a8 = (__entry_r125 - 0x1051);
      var_14b0 = (__entry_r125 - 0x1054);
      var_14b8 = (__entry_r125 - 0x1158);
      var_14c8 = (__entry_r125 - 0x1056);
      var_14c0 = (__entry_r125 - 0x1057);
      var_14d0 = (__entry_r125 - 0x1055);
      var_14d8 = (__entry_r125 - 0x1058);
      var_14e0 = (__entry_r125 - 0x115c);
      var_14e8 = (__entry_r125 - 0x105b);
      var_14f0 = (__entry_r125 - 0x105a);
      var_14f8 = (__entry_r125 - 0x1059);
      var_1500 = (__entry_r125 - 0x105c);
      var_1508 = (__entry_r125 - 0x1160);
      var_1510 = (__entry_r125 - 0x105f);
      var_1518 = (__entry_r125 - 0x105e);
      var_1520 = (__entry_r125 - 0x105d);
      var_1528 = (__entry_r125 - 0x1060);
      var_1530 = (__entry_r125 - 0x1164);
      var_1538 = (__entry_r125 - 0x1063);
      var_1540 = (__entry_r125 - 0x1062);
      var_1548 = (__entry_r125 - 0x1061);
      var_1550 = (__entry_r125 - 0x1064);
      var_1560 = (__entry_r125 - 0x1067);
      var_1558 = (__entry_r125 - 0x1168);
      var_1568 = (__entry_r125 - 0x1066);
      var_1570 = (__entry_r125 - 0x1065);
      var_1578 = (__entry_r125 - 0x1068);
      var_1580 = (__entry_r125 - 0x116c);
      var_1588 = (__entry_r125 - 0x106b);
      var_1590 = (__entry_r125 - 0x106a);
      var_1598 = (__entry_r125 - 0x1069);
      var_15a0 = (__entry_r125 - 0x106c);
      var_15a8 = (__entry_r125 - 0x1170);
      var_15b0 = (__entry_r125 - 0x106f);
      var_15b8 = (__entry_r125 - 0x106e);
      var_15c0 = (__entry_r125 - 0x106d);
      var_15c8 = (__entry_r125 - 0x1070);
      var_15d0 = (__entry_r125 - 0x1174);
      var_15d8 = (__entry_r125 - 0x1073);
      var_15e0 = (__entry_r125 - 0x1072);
      var_15e8 = (__entry_r125 - 0x1071);
      var_15f0 = (__entry_r125 - 0x1074);
      var_15f8 = (__entry_r125 - 0x1178);
      var_1600 = (__entry_r125 - 0x1077);
      var_1608 = (__entry_r125 - 0x1076);
      var_1610 = (__entry_r125 - 0x1075);
      var_1618 = (__entry_r125 - 0x1078);
      var_1620 = (__entry_r125 - 0x117c);
      _cse924 = (__entry_r125 - 0x1080);
      var_1628 = (_cse924 | 0x5);
      var_1630 = (_cse924 | 0x6);
      var_1638 = (_cse924 | 0x7);
      var_1640 = (_cse924 | 0x4);
      var_1648 = (__entry_r125 - 0x1180);
      var_1650 = (_cse924 | 0x1);
      var_1658 = (_cse924 | 0x2);
      var_1660 = (_cse924 | 0x3);
      var_1668 = var_1648;
      var_1670 = (__entry_r125 - 0x1048);
      var_1678 = (__entry_r125 - 0x1047);
      var_1680 = (__entry_r125 - 0x1046);
      var_1688 = (__entry_r125 - 0x1045);
      var_1690 = (__entry_r125 - 0x1044);
      var_1698 = (__entry_r125 - 0x1043);
      var_16a0 = (__entry_r125 - 0x1042);
      var_16a8 = (__entry_r125 - 0x1041);
      var_16ac = ((uint32_t)(var_1a98));
      var_16b8 = var_1a98;
      goto L_0x10004d40;
    }
    // b170 @0x10007a68
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x667f4d22) == 0x1)) {
      // b172 @0x10007a84
      t185 = var_13d8;
      t186 = var_1a98;
      t187 = var_13d0;
      t188 = var_13c8;
      (*(uint8_t*)(t188)) = (*(uint8_t*)(t187));
      var_13c8 = (t188 - 0x1);
      var_13d0 = (t187 - 0x1);
      var_13d8 = (t185 - 0x1);
      if (((var_13d8 == t186) == 0x1)) {
        var_1364 = 0xffffffff89fd6da1;
      } else {
        var_1364 = 0x667f4d22;
      }
      t0 = (t188 - 0x1);
      t1 = t188;
    }
    goto L_0x10004d40;
L_0x1000707c:
    // b96 @0x1000707c
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x68901447) == 0x1)) {
      // b288 @0x1000c194
      /* 1 argument slot(s) hold values this function never wrote: either the callee reads a stale register or a definition was lost here */
      t426 = vm_memset(((__entry_r125 - 0x1250) + var_500), 0x0, (0x37 - var_508), 0x10016a2c, t1, t2, t3, t4);
      var_1364 = 0x263afc19;
      t0 = t1 = t2 = t3 = t4 = 0x0; /* t0, t1, t2, t3, t4: never set up on this edge */
      goto L_0x10004d40;
    }
    // b98 @0x10007098
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x6912374d) == 0x1)) {
      // b100 @0x100070b4
      t165 = var_3ec;
      var_798 = (_cse925 & 0xffffffff);
      _cse925 = ((int64_t)(int32_t)(t165));
      (*(uint8_t*)(((__entry_r125 - 0x11f0) + var_798))) = 0xfc;
      var_79c = ((uint32_t)((_cse925 + 0x1)));
      if (((var_798 < 0x38) == 0x1)) {
        var_1364 = 0xffffffff93092d53;
      } else {
        var_1364 = 0x4433ffef;
      }
      t0 = 0xffffffff93092d53;
    }
    goto L_0x10004d40;
L_0x10008db0:
    // b224 @0x10008db0
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x6e8cd62a) == 0x1)) {
      // b320 @0x10013d94
      t834 = var_1ad0;
      var_ff8 = (__entry_r125 - 0x12d0);
      var_ff0 = (__entry_r125 - 0x1310);
      _cse926 = ((int64_t)(int8_t)(var_1281));
      var_12c1 = ((uint8_t)((_cse926 ^ 0xdf)));
      _cse927 = ((int64_t)(int8_t)(var_1282));
      var_12c2 = ((uint8_t)((_cse927 ^ 0xdf)));
      _cse928 = ((int64_t)(int8_t)(var_1283));
      var_12c3 = ((uint8_t)((_cse928 ^ 0xdf)));
      _cse929 = ((int64_t)(int8_t)(var_1284));
      var_12c4 = ((uint8_t)((_cse929 ^ 0xdf)));
      _cse930 = ((int64_t)(int8_t)(var_1285));
      var_12c5 = ((uint8_t)((_cse930 ^ 0xdf)));
      _cse931 = ((int64_t)(int8_t)(var_1286));
      var_12c6 = ((uint8_t)((_cse931 ^ 0xdf)));
      _cse932 = ((int64_t)(int8_t)(var_1287));
      var_12c7 = ((uint8_t)((_cse932 ^ 0xdf)));
      _cse933 = ((int64_t)(int8_t)((*(uint8_t*)(&var_1288))));
      (*(uint8_t*)(&var_12c8)) = ((uint8_t)((_cse933 ^ 0xdf)));
      _cse934 = ((int64_t)(int8_t)(var_1289));
      var_12c9 = ((uint8_t)((_cse934 ^ 0xdf)));
      _cse935 = ((int64_t)(int8_t)(var_128a));
      var_12ca = ((uint8_t)((_cse935 ^ 0xdf)));
      _cse936 = ((int64_t)(int8_t)(var_128b));
      var_12cb = ((uint8_t)((_cse936 ^ 0xdf)));
      _cse937 = ((int64_t)(int8_t)(var_128c));
      var_12cc = ((uint8_t)((_cse937 ^ 0xdf)));
      _cse938 = ((int64_t)(int8_t)(var_128d));
      var_12cd = ((uint8_t)((_cse938 ^ 0xdf)));
      _cse939 = ((int64_t)(int8_t)(var_128e));
      var_12ce = ((uint8_t)((_cse939 ^ 0xdf)));
      _cse940 = ((int64_t)(int8_t)(var_128f));
      var_12cf = ((uint8_t)((_cse940 ^ 0xdf)));
      _cse941 = ((int64_t)(int8_t)((*(uint8_t*)(&var_1290))));
      (*(uint8_t*)(&var_12d0)) = ((uint8_t)((_cse941 ^ 0xdf)));
      var_1301 = ((uint8_t)((_cse926 ^ 0xec)));
      var_1302 = ((uint8_t)((_cse927 ^ 0xec)));
      var_1303 = ((uint8_t)((_cse928 ^ 0xec)));
      var_1304 = ((uint8_t)((_cse929 ^ 0xec)));
      var_1305 = ((uint8_t)((_cse930 ^ 0xec)));
      var_1306 = ((uint8_t)((_cse931 ^ 0xec)));
      var_1307 = ((uint8_t)((_cse932 ^ 0xec)));
      (*(uint8_t*)(&var_1308)) = ((uint8_t)((_cse933 ^ 0xec)));
      var_1309 = ((uint8_t)((_cse934 ^ 0xec)));
      var_130a = ((uint8_t)((_cse935 ^ 0xec)));
      var_130b = ((uint8_t)((_cse936 ^ 0xec)));
      var_130c = ((uint8_t)((_cse937 ^ 0xec)));
      var_130d = ((uint8_t)((_cse938 ^ 0xec)));
      var_130e = ((uint8_t)((_cse939 ^ 0xec)));
      var_130f = ((uint8_t)((_cse940 ^ 0xec)));
      (*(uint8_t*)(&var_1310)) = ((uint8_t)((_cse941 ^ 0xec)));
      _cse942 = ((int64_t)(int8_t)(var_1271));
      var_12b1 = ((uint8_t)((_cse942 ^ 0xdf)));
      _cse943 = ((int64_t)(int8_t)(var_1272));
      var_12b2 = ((uint8_t)((_cse943 ^ 0xdf)));
      _cse944 = ((int64_t)(int8_t)(var_1273));
      var_12b3 = ((uint8_t)((_cse944 ^ 0xdf)));
      _cse945 = ((int64_t)(int8_t)(var_1274));
      var_12b4 = ((uint8_t)((_cse945 ^ 0xdf)));
      _cse946 = ((int64_t)(int8_t)(var_1275));
      var_12b5 = ((uint8_t)((_cse946 ^ 0xdf)));
      _cse947 = ((int64_t)(int8_t)(var_1276));
      var_12b6 = ((uint8_t)((_cse947 ^ 0xdf)));
      _cse948 = ((int64_t)(int8_t)(var_1277));
      var_12b7 = ((uint8_t)((_cse948 ^ 0xdf)));
      _cse949 = ((int64_t)(int8_t)((*(uint8_t*)(&var_1278))));
      (*(uint8_t*)(&var_12b8)) = ((uint8_t)((_cse949 ^ 0xdf)));
      _cse950 = ((int64_t)(int8_t)(var_1279));
      var_12b9 = ((uint8_t)((_cse950 ^ 0xdf)));
      _cse951 = ((int64_t)(int8_t)(var_127a));
      var_12ba = ((uint8_t)((_cse951 ^ 0xdf)));
      _cse952 = ((int64_t)(int8_t)(var_127b));
      var_12bb = ((uint8_t)((_cse952 ^ 0xdf)));
      _cse953 = ((int64_t)(int8_t)(var_127c));
      var_12bc = ((uint8_t)((_cse953 ^ 0xdf)));
      _cse954 = ((int64_t)(int8_t)(var_127d));
      var_12bd = ((uint8_t)((_cse954 ^ 0xdf)));
      _cse955 = ((int64_t)(int8_t)(var_127e));
      var_12be = ((uint8_t)((_cse955 ^ 0xdf)));
      _cse956 = ((int64_t)(int8_t)(var_127f));
      var_12bf = ((uint8_t)((_cse956 ^ 0xdf)));
      _cse957 = ((int64_t)(int8_t)((*(uint8_t*)(&var_1280))));
      (*(uint8_t*)(&var_12c0)) = ((uint8_t)((_cse957 ^ 0xdf)));
      var_12f1 = ((uint8_t)((_cse942 ^ 0xec)));
      var_12f2 = ((uint8_t)((_cse943 ^ 0xec)));
      var_12f3 = ((uint8_t)((_cse944 ^ 0xec)));
      var_12f4 = ((uint8_t)((_cse945 ^ 0xec)));
      var_12f5 = ((uint8_t)((_cse946 ^ 0xec)));
      var_12f6 = ((uint8_t)((_cse947 ^ 0xec)));
      var_12f7 = ((uint8_t)((_cse948 ^ 0xec)));
      (*(uint8_t*)(&var_12f8)) = ((uint8_t)((_cse949 ^ 0xec)));
      var_12f9 = ((uint8_t)((_cse950 ^ 0xec)));
      var_12fa = ((uint8_t)((_cse951 ^ 0xec)));
      var_12fb = ((uint8_t)((_cse952 ^ 0xec)));
      var_12fc = ((uint8_t)((_cse953 ^ 0xec)));
      var_12fd = ((uint8_t)((_cse954 ^ 0xec)));
      var_12fe = ((uint8_t)((_cse955 ^ 0xec)));
      var_12ff = ((uint8_t)((_cse956 ^ 0xec)));
      (*(uint8_t*)(&var_1300)) = ((uint8_t)((_cse957 ^ 0xec)));
      _cse958 = ((int64_t)(int8_t)(var_1261));
      var_12a1 = ((uint8_t)((_cse958 ^ 0xdf)));
      _cse959 = ((int64_t)(int8_t)(var_1262));
      var_12a2 = ((uint8_t)((_cse959 ^ 0xdf)));
      _cse960 = ((int64_t)(int8_t)(var_1263));
      var_12a3 = ((uint8_t)((_cse960 ^ 0xdf)));
      _cse961 = ((int64_t)(int8_t)(var_1264));
      var_12a4 = ((uint8_t)((_cse961 ^ 0xdf)));
      _cse962 = ((int64_t)(int8_t)(var_1265));
      var_12a5 = ((uint8_t)((_cse962 ^ 0xdf)));
      _cse963 = ((int64_t)(int8_t)(var_1266));
      var_12a6 = ((uint8_t)((_cse963 ^ 0xdf)));
      _cse964 = ((int64_t)(int8_t)(var_1267));
      var_12a7 = ((uint8_t)((_cse964 ^ 0xdf)));
      _cse965 = ((int64_t)(int8_t)((*(uint8_t*)(&var_1268))));
      (*(uint8_t*)(&var_12a8)) = ((uint8_t)((_cse965 ^ 0xdf)));
      _cse966 = ((int64_t)(int8_t)(var_1269));
      var_12a9 = ((uint8_t)((_cse966 ^ 0xdf)));
      _cse967 = ((int64_t)(int8_t)(var_126a));
      var_12aa = ((uint8_t)((_cse967 ^ 0xdf)));
      _cse968 = ((int64_t)(int8_t)(var_126b));
      var_12ab = ((uint8_t)((_cse968 ^ 0xdf)));
      _cse969 = ((int64_t)(int8_t)(var_126c));
      var_12ac = ((uint8_t)((_cse969 ^ 0xdf)));
      _cse970 = ((int64_t)(int8_t)(var_126d));
      var_12ad = ((uint8_t)((_cse970 ^ 0xdf)));
      _cse971 = ((int64_t)(int8_t)(var_126e));
      var_12ae = ((uint8_t)((_cse971 ^ 0xdf)));
      _cse972 = ((int64_t)(int8_t)(var_126f));
      var_12af = ((uint8_t)((_cse972 ^ 0xdf)));
      _cse973 = ((int64_t)(int8_t)((*(uint8_t*)(&var_1270))));
      (*(uint8_t*)(&var_12b0)) = ((uint8_t)((_cse973 ^ 0xdf)));
      var_12e1 = ((uint8_t)((_cse958 ^ 0xec)));
      var_12e2 = ((uint8_t)((_cse959 ^ 0xec)));
      var_12e3 = ((uint8_t)((_cse960 ^ 0xec)));
      var_12e4 = ((uint8_t)((_cse961 ^ 0xec)));
      var_12e5 = ((uint8_t)((_cse962 ^ 0xec)));
      var_12e6 = ((uint8_t)((_cse963 ^ 0xec)));
      var_12e7 = ((uint8_t)((_cse964 ^ 0xec)));
      (*(uint8_t*)(&var_12e8)) = ((uint8_t)((_cse965 ^ 0xec)));
      var_12e9 = ((uint8_t)((_cse966 ^ 0xec)));
      var_12ea = ((uint8_t)((_cse967 ^ 0xec)));
      var_12eb = ((uint8_t)((_cse968 ^ 0xec)));
      var_12ec = ((uint8_t)((_cse969 ^ 0xec)));
      var_12ed = ((uint8_t)((_cse970 ^ 0xec)));
      var_12ee = ((uint8_t)((_cse971 ^ 0xec)));
      var_12ef = ((uint8_t)((_cse972 ^ 0xec)));
      (*(uint8_t*)(&var_12f0)) = ((uint8_t)((_cse973 ^ 0xec)));
      _cse974 = ((int64_t)(int8_t)(var_1251));
      var_1291 = ((uint8_t)((_cse974 ^ 0xdf)));
      _cse975 = ((int64_t)(int8_t)(var_1252));
      var_1292 = ((uint8_t)((_cse975 ^ 0xdf)));
      t835 = var_1253;
      _cse976 = ((int64_t)(int8_t)(t835));
      var_1293 = ((uint8_t)((_cse976 ^ 0xdf)));
      t836 = var_1254;
      _cse977 = ((int64_t)(int8_t)(t836));
      var_1294 = ((uint8_t)((_cse977 ^ 0xdf)));
      t837 = var_1255;
      _cse978 = ((int64_t)(int8_t)(t837));
      var_1295 = ((uint8_t)((_cse978 ^ 0xdf)));
      t838 = var_1256;
      _cse979 = ((int64_t)(int8_t)(t838));
      var_1296 = ((uint8_t)((_cse979 ^ 0xdf)));
      t839 = var_1257;
      _cse980 = ((int64_t)(int8_t)(t839));
      var_1297 = ((uint8_t)((_cse980 ^ 0xdf)));
      _cse981 = ((int64_t)(int8_t)((*(uint8_t*)(&var_1258))));
      (*(uint8_t*)(&var_1298)) = ((uint8_t)((_cse981 ^ 0xdf)));
      _cse982 = ((int64_t)(int8_t)(var_1259));
      var_1299 = ((uint8_t)((_cse982 ^ 0xdf)));
      _cse983 = ((int64_t)(int8_t)(var_125a));
      var_129a = ((uint8_t)((_cse983 ^ 0xdf)));
      _cse984 = ((int64_t)(int8_t)(var_125b));
      var_129b = ((uint8_t)((_cse984 ^ 0xdf)));
      _cse985 = ((int64_t)(int8_t)(var_125c));
      var_129c = ((uint8_t)((_cse985 ^ 0xdf)));
      _cse986 = ((int64_t)(int8_t)(var_125d));
      var_129d = ((uint8_t)((_cse986 ^ 0xdf)));
      _cse987 = ((int64_t)(int8_t)(var_125e));
      var_129e = ((uint8_t)((_cse987 ^ 0xdf)));
      _cse988 = ((int64_t)(int8_t)(var_125f));
      var_129f = ((uint8_t)((_cse988 ^ 0xdf)));
      _cse989 = ((int64_t)(int8_t)((*(uint8_t*)(&var_1260))));
      (*(uint8_t*)(&var_12a0)) = ((uint8_t)((_cse989 ^ 0xdf)));
      var_12d1 = ((uint8_t)((_cse974 ^ 0xec)));
      var_12d2 = ((uint8_t)((_cse975 ^ 0xec)));
      var_12d3 = ((uint8_t)((_cse976 ^ 0xec)));
      var_12d4 = ((uint8_t)((_cse977 ^ 0xec)));
      var_12d5 = ((uint8_t)((_cse978 ^ 0xec)));
      var_12d6 = ((uint8_t)((_cse979 ^ 0xec)));
      var_12d7 = ((uint8_t)((_cse980 ^ 0xec)));
      (*(uint8_t*)(&var_12d8)) = ((uint8_t)((_cse981 ^ 0xec)));
      var_12d9 = ((uint8_t)((_cse982 ^ 0xec)));
      var_12da = ((uint8_t)((_cse983 ^ 0xec)));
      var_12db = ((uint8_t)((_cse984 ^ 0xec)));
      var_12dc = ((uint8_t)((_cse985 ^ 0xec)));
      var_12dd = ((uint8_t)((_cse986 ^ 0xec)));
      var_12de = ((uint8_t)((_cse987 ^ 0xec)));
      var_12df = ((uint8_t)((_cse988 ^ 0xec)));
      (*(uint8_t*)(&var_12e0)) = ((uint8_t)((_cse989 ^ 0xec)));
      var_fe8 = t7;
      var_fd8 = (uint8_t*)(__entry_r125 - 0x1330);
      var_fe0 = ((t7 - (((((0x20 < t834) == 0x1) ? t834 : 0x20) + 0x47) & 0xfffffffffffffff8)) & 0xfffffffffffffff0);
      if (((var_fe0 < (uint64_t)(&var_1310)) == 0x1)) {
        var_1364 = 0x6845caef;
      } else {
        var_1364 = 0xffffffffe3c9c744;
      }
      t0 = ((int64_t)(int8_t)(t835));
      t1 = ((int64_t)(int8_t)(t836));
      t2 = ((int64_t)(int8_t)(t837));
      t3 = ((int64_t)(int8_t)(t838));
      t4 = ((int64_t)(int8_t)(t839));
      t7 = ((t7 - (((((0x20 < t834) == 0x1) ? t834 : 0x20) + 0x47) & 0xfffffffffffffff8)) & 0xfffffffffffffff0);
      goto L_0x10004d40;
    }
    // b226 @0x10008dcc
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x839fc771) == 0x1)) {
      // b228 @0x10008de8
      var_1364 = 0x667f4d22;
      var_13c8 = var_13b0;
      var_13d0 = var_13b8;
      var_13d8 = var_13c0;
    }
    goto L_0x10004d40;
L_0x10004d78:
    // b5 @0x10004d78
    if ((((int64_t)(0xa8246970) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b7 @0x10004d94
      if ((((int64_t)(0xb47a9ad3) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) != 0x1)) {
        goto L_0x10004db0;
      }
      // b101 @0x1000712c
      if ((((int64_t)(0xbc64ede1) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
        // b229 @0x10008e50
        if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xbc64ede2) == 0x1)) {
          // b321 @0x10014994
          t840 = var_fa0;
          t841 = var_13a8;
          _cse990 = (var_f98 - t841);
          t842 = (*(uint8_t*)((_cse990 - 0xf)));
          t843 = (*(uint8_t*)((_cse990 - 0xe)));
          t844 = (*(uint8_t*)((_cse990 - 0xd)));
          t845 = (*(uint8_t*)((_cse990 - 0xc)));
          t846 = (*(uint8_t*)((_cse990 - 0xb)));
          t847 = (*(uint8_t*)((_cse990 - 0xa)));
          t848 = (*(uint8_t*)((_cse990 - 0x9)));
          t849 = (*(uint8_t*)((_cse990 - 0x8)));
          t850 = (*(uint8_t*)((_cse990 - 0x7)));
          t851 = (*(uint8_t*)((_cse990 - 0x6)));
          t852 = (*(uint8_t*)((_cse990 - 0x5)));
          t853 = (*(uint8_t*)((_cse990 - 0x4)));
          t854 = (*(uint8_t*)((_cse990 - 0x3)));
          t855 = (*(uint8_t*)((_cse990 - 0x2)));
          t856 = (*(uint8_t*)((_cse990 - 0x1)));
          t857 = (*(uint8_t*)(_cse990));
          t858 = (*(uint8_t*)((_cse990 - 0x1f)));
          t859 = (*(uint8_t*)((_cse990 - 0x1e)));
          t860 = (*(uint8_t*)((_cse990 - 0x1d)));
          t861 = (*(uint8_t*)((_cse990 - 0x1c)));
          t862 = (*(uint8_t*)((_cse990 - 0x1b)));
          t863 = (*(uint8_t*)((_cse990 - 0x1a)));
          t864 = (*(uint8_t*)((_cse990 - 0x19)));
          t865 = (*(uint8_t*)((_cse990 - 0x18)));
          t866 = (*(uint8_t*)((_cse990 - 0x17)));
          t867 = (*(uint8_t*)((_cse990 - 0x16)));
          t868 = (*(uint8_t*)((_cse990 - 0x15)));
          t869 = (*(uint8_t*)((_cse990 - 0x14)));
          t870 = (*(uint8_t*)((_cse990 - 0x13)));
          t871 = (*(uint8_t*)((_cse990 - 0x12)));
          t872 = (*(uint8_t*)((_cse990 - 0x11)));
          _cse991 = (t840 - t841);
          (*(uint8_t*)((_cse991 - 0x10))) = (*(uint8_t*)((_cse990 - 0x10)));
          (*(uint8_t*)((_cse991 - 0x11))) = t872;
          (*(uint8_t*)((_cse991 - 0x12))) = t871;
          (*(uint8_t*)((_cse991 - 0x13))) = t870;
          (*(uint8_t*)((_cse991 - 0x14))) = t869;
          (*(uint8_t*)((_cse991 - 0x15))) = t868;
          (*(uint8_t*)((_cse991 - 0x16))) = t867;
          (*(uint8_t*)((_cse991 - 0x17))) = t866;
          (*(uint8_t*)((_cse991 - 0x18))) = t865;
          (*(uint8_t*)((_cse991 - 0x19))) = t864;
          (*(uint8_t*)((_cse991 - 0x1a))) = t863;
          (*(uint8_t*)((_cse991 - 0x1b))) = t862;
          (*(uint8_t*)((_cse991 - 0x1c))) = t861;
          (*(uint8_t*)((_cse991 - 0x1d))) = t860;
          (*(uint8_t*)((_cse991 - 0x1e))) = t859;
          (*(uint8_t*)((_cse991 - 0x1f))) = t858;
          (*(uint8_t*)(_cse991)) = t857;
          (*(uint8_t*)((_cse991 - 0x1))) = t856;
          (*(uint8_t*)((_cse991 - 0x2))) = t855;
          (*(uint8_t*)((_cse991 - 0x3))) = t854;
          (*(uint8_t*)((_cse991 - 0x4))) = t853;
          (*(uint8_t*)((_cse991 - 0x5))) = t852;
          (*(uint8_t*)((_cse991 - 0x6))) = t851;
          (*(uint8_t*)((_cse991 - 0x7))) = t850;
          (*(uint8_t*)((_cse991 - 0x8))) = t849;
          (*(uint8_t*)((_cse991 - 0x9))) = t848;
          (*(uint8_t*)((_cse991 - 0xa))) = t847;
          (*(uint8_t*)((_cse991 - 0xb))) = t846;
          (*(uint8_t*)((_cse991 - 0xc))) = t845;
          (*(uint8_t*)((_cse991 - 0xd))) = t844;
          (*(uint8_t*)((_cse991 - 0xe))) = t843;
          (*(uint8_t*)((_cse991 - 0xf))) = t842;
          t873 = var_1b00;
          var_13a8 = (t841 + 0x20);
          if (((var_13a8 == var_f88) == 0x1)) {
            var_1364 = t873;
          } else {
            var_1364 = 0xffffffffbc64ede2;
          }
          t0 = t873;
          t1 = ((int64_t)(int8_t)(t843));
          t2 = ((int64_t)(int8_t)(t844));
          t3 = ((int64_t)(int8_t)(t845));
          t4 = ((int64_t)(int8_t)(t846));
          goto L_0x10004d40;
        }
        goto L_0x10008e6c;
      }
      // b103 @0x10007148
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xb47a9ad4) != 0x1)) {
        goto L_0x10007164;
      }
      // b289 @0x1000c1e8
      (*var_1010) = var_4f3;
      var_1364 = 0x5e619a9b;
      var_1a78 = (uint8_t*)&var_1351;
      var_1354 = var_1334;
      var_1356 = var_1336;
      var_1358 = var_1338;
      var_135a = var_133a;
      var_135f = var_4f0;
      var_1352 = var_1332;
      var_1351 = var_1331;
      var_135e = var_4ed;
      var_135d = var_4eb;
      var_135c = var_4f2;
      var_135b = var_4ef;
      goto L_0x10004d40;
    }
    // b57 @0x10006788
    if ((((int64_t)(0x93092d52) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b59 @0x100067a4
      if ((((int64_t)(0xa259f5ad) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) != 0x1)) {
        // b61 @0x100067c0
        if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x93092d53) == 0x1)) {
          // b281 @0x1000ba34
          if ((((((int64_t)(int32_t)(var_79c)) & 0xffffffff) < 0x38) == 0x1)) {
            var_1364 = 0xffffffffc94e0219;
          } else {
            var_1364 = var_1ae8;
          }
          goto L_0x10004d40;
        } else {
          goto L_0x100067dc;
        }
      }
      // b201 @0x1000818c
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xa259f5ae) == 0x1)) {
        // b313 @0x10011f38
        var_3b8 = var_1a68;
        (*(uint8_t*)(((__entry_r125 - 0x1250) + (((int64_t)(int32_t)(var_1a60)) & 0xffffffff)))) = (*(uint8_t*)(((__entry_r125 - 0x1330) + var_1a68)));
        t680 = (*var_730);
        t681 = var_1aa8;
        _cse993 = (((int64_t)(int32_t)(t680)) + 0x1);
        _cse992 = ((uint32_t)(_cse993));
        (*var_730) = _cse992;
        var_1a6c = _cse992;
        if ((((_cse993 & 0xffffffff) == t681) == 0x1)) {
          var_1364 = 0x177db653;
        } else {
          var_1364 = 0x180ab919;
        }
        goto L_0x10004d40;
      }
      goto L_0x100081a8;
    }
    // b137 @0x100076b0
    if ((((int64_t)(0x89a592fe) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b139 @0x100076cc
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x89a592ff) != 0x1)) {
        goto L_0x100076e8;
      }
      // b297 @0x1000ce04
      if ((((((int64_t)(int32_t)(var_c64)) & 0xffffffff) < 0x38) == 0x1)) {
        var_1364 = 0xffffffffe0dad150;
      } else {
        var_1364 = 0x19b80fb0;
      }
      var_1d38 = var_f10;
      var_1d40 = var_f08;
      var_1d48 = var_f00;
      var_1d50 = var_ef8;
      var_1d58 = var_ef0;
      t472 = var_da0;
      t473 = var_ce0;
      t474 = var_cd8;
      t475 = var_cc0;
      var_1430 = var_c98;
      var_1438 = t475;
      var_1440 = var_ce8;
      var_1448 = var_cf0;
      var_1450 = var_cf8;
      var_1458 = var_d00;
      var_1460 = var_d08;
      var_1468 = var_d10;
      var_1470 = var_d18;
      var_1478 = var_d20;
      var_1480 = var_d28;
      var_1488 = var_d30;
      var_1490 = var_d38;
      var_1498 = var_d40;
      var_14a0 = var_d48;
      var_14a8 = var_d50;
      var_14b0 = var_d58;
      var_14b8 = var_d60;
      var_14c0 = var_d68;
      var_14c8 = var_d70;
      var_14d0 = var_d78;
      var_14d8 = var_d80;
      var_14e0 = var_d88;
      var_14e8 = var_d90;
      var_14f0 = var_d98;
      var_14f8 = t472;
      var_1500 = var_da8;
      var_1508 = var_db0;
      var_1510 = var_db8;
      var_1518 = var_dc0;
      var_1520 = var_dc8;
      var_1528 = var_dd0;
      var_1530 = var_dd8;
      var_1538 = var_de0;
      var_1540 = var_de8;
      var_1548 = var_df0;
      var_1550 = var_df8;
      var_1558 = var_e00;
      var_1560 = var_e08;
      var_1568 = var_e10;
      var_1570 = var_e18;
      var_1578 = var_e20;
      var_1580 = var_e28;
      var_1588 = var_e30;
      var_1590 = var_e38;
      var_1598 = var_e40;
      var_15a0 = var_e48;
      var_15a8 = var_e50;
      var_15b0 = var_e58;
      var_15b8 = var_e60;
      var_15c0 = var_e68;
      var_15c8 = var_e70;
      var_15d0 = var_e78;
      var_15d8 = var_e80;
      var_15e0 = var_e88;
      var_15e8 = var_e90;
      var_15f0 = var_e98;
      var_15f8 = var_ea0;
      var_1600 = var_ea8;
      var_1608 = var_eb0;
      var_1610 = var_eb8;
      var_1618 = var_ec0;
      var_1620 = var_ec8;
      var_1628 = var_ed0;
      var_1630 = var_ed8;
      var_1638 = var_ee0;
      var_1640 = var_ee8;
      var_1648 = var_ef0;
      var_1650 = var_ef8;
      var_1658 = var_f00;
      var_1660 = var_f08;
      var_1668 = var_f10;
      var_1670 = t473;
      var_1678 = var_cc8;
      var_1680 = var_cd0;
      var_1688 = t474;
      var_1690 = var_cb8;
      var_1698 = var_ca0;
      var_16a0 = var_ca8;
      var_16a8 = var_cb0;
      var_16ac = var_4cc;
      var_16b8 = var_c60;
      var_16f8 = var_c98;
      var_1700 = t475;
      var_1708 = var_ce8;
      var_1710 = var_cf0;
      var_1718 = var_cf8;
      var_1720 = var_d00;
      var_1728 = var_d08;
      var_1730 = var_d10;
      var_1738 = var_d18;
      var_1740 = var_d20;
      var_1748 = var_d28;
      var_1750 = var_d30;
      var_1758 = var_d38;
      var_1760 = var_d40;
      var_1768 = var_d48;
      var_1770 = var_d50;
      var_1778 = var_d58;
      var_1780 = var_d60;
      var_1788 = var_d68;
      var_1790 = var_d70;
      var_1798 = var_d78;
      var_17a0 = var_d80;
      var_17a8 = var_d88;
      var_17b0 = var_d90;
      var_17b8 = var_d98;
      var_17c0 = t472;
      var_17c8 = var_da8;
      var_17d0 = var_db0;
      var_17d8 = var_db8;
      var_17e0 = var_dc0;
      var_17e8 = var_dc8;
      var_17f0 = var_dd0;
      var_17f8 = var_dd8;
      var_1800 = var_de0;
      var_1808 = var_de8;
      var_1810 = var_df0;
      var_1818 = var_df8;
      var_1820 = var_e00;
      var_1828 = var_e08;
      var_1830 = var_e10;
      var_1838 = var_e18;
      var_1840 = var_e20;
      var_1848 = var_e28;
      var_1850 = var_e30;
      var_1858 = var_e38;
      var_1860 = var_e40;
      var_1868 = var_e48;
      var_1870 = var_e50;
      var_1878 = var_e58;
      var_1880 = var_e60;
      var_1888 = var_e68;
      var_1890 = var_e70;
      var_1898 = var_e78;
      var_18a0 = var_e80;
      var_18a8 = var_e88;
      var_18b0 = var_e90;
      var_18b8 = var_e98;
      var_18c0 = var_ea0;
      var_18c8 = var_ea8;
      var_18d0 = var_eb0;
      var_18d8 = var_eb8;
      var_18e0 = var_ec0;
      var_18e8 = var_ec8;
      var_18f0 = var_ed0;
      var_18f8 = var_ed8;
      var_1900 = var_ee0;
      var_1908 = var_ee8;
      var_1910 = var_ef0;
      var_1918 = var_ef8;
      var_1920 = var_f00;
      var_1928 = var_f08;
      var_1930 = var_f10;
      var_1938 = t473;
      var_1940 = var_cc8;
      var_1948 = var_cd0;
      var_1950 = t474;
      var_1958 = var_cb8;
      var_1960 = var_ca0;
      var_1968 = var_ca8;
      var_1970 = var_cb0;
      t0 = 0x852eddc2;
      t1 = t473;
      t2 = t474;
      t3 = t472;
      t4 = t475;
      goto L_0x10004d40;
    }
    // b255 @0x10009960
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x852eddc3) == 0x1)) {
      // b328 @0x10015a44
      var_fa8 = (var_1ad0 - 0x1);
      if (((var_1ad0 == var_1a98) == 0x1)) {
        var_1364 = 0xffffffff89fd6da1;
      } else {
        var_1364 = 0xffffffffe8038afc;
      }
      goto L_0x10004d40;
    }
    // b257 @0x1000997c
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x8818e777) == 0x1)) {
      // b259 @0x10009998
      /* 1 argument slot(s) hold values this function never wrote: either the callee reads a stale register or a definition was lost here */
      t293 = vm_memset(((__entry_r125 - 0x11f0) + (((int64_t)(int32_t)(var_79c)) & 0xffffffff)), 0x0, (((0x3e - ((int64_t)(int32_t)(var_3ec))) & 0xffffffff) + 0x1), 0x10016a2c, t1, t2, t3, t4);
      var_1364 = 0xc0a982cf;
      t0 = t1 = t2 = t3 = t4 = 0x0; /* t0, t1, t2, t3, t4: never set up on this edge */
    }
    goto L_0x10004d40;
L_0x100076e8:
    // b141 @0x100076e8
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x89fd6da1) == 0x1)) {
      // b298 @0x1000e5a8
      var_f60 = (uint32_t*)&var_1040;
      var_f70 = (var_1ad0 + 0x40);
      var_f68 = (uint8_t*)(__entry_r125 - 0x1080);
      t476 = var_1a98;
      var_1040 = ((uint32_t)(t476));
      var_f58 = (uint64_t*)&var_1038;
      var_1038 = t476;
      var_f50 = (uint32_t*)&var_1030;
      var_f48 = (uint32_t*)(__entry_r125 - 0x102c);
      var_f40 = (uint32_t*)&var_1028;
      var_f38 = (uint32_t*)(__entry_r125 - 0x1024);
      var_1028 = 0x5f96bc2640fcca21;
      var_1030 = 0x55c746b4a195712e;
      var_f30 = (uint32_t*)&var_1020;
      var_f28 = (uint32_t*)(__entry_r125 - 0x101c);
      var_f20 = (uint32_t*)&var_1018;
      var_f18 = (uint32_t*)(__entry_r125 - 0x1014);
      var_1018 = 0x52a98cf79b113ee0;
      var_1020 = 0xc54dc1c7d18a047b;
      if (((var_f70 == t476) == 0x1)) {
        var_1364 = 0x6326e7c8;
      } else {
        var_1364 = 0xffffffffcf0494cd;
      }
      t0 = t476;
      goto L_0x10004d40;
    }
    // b143 @0x10007704
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x8a19f97e) == 0x1)) {
      // b145 @0x10007720
      t175 = (*var_a00);
      var_768 = t175;
      t176 = (*var_9f8);
      var_764 = t176;
      t177 = (*var_9f0);
      var_760 = t177;
      t178 = (*var_9e8);
      var_75c = t178;
      t179 = (*var_9e0);
      var_758 = t179;
      t180 = (*var_9d8);
      var_754 = t180;
      t181 = (*var_9d0);
      var_750 = t181;
      t182 = (*var_9c8);
      var_74c = t182;
      var_1364 = 0x3083817c;
      var_1a38 = var_1a98;
      var_1a44 = ((uint32_t)(var_1a98));
      var_1a3c = t175;
      var_1a40 = t176;
      var_1a58 = t178;
      var_1a54 = t179;
      var_1a50 = t180;
      var_1a4c = t181;
      var_1a48 = t182;
      var_1a5c = t177;
    }
    goto L_0x10004d40;
L_0x100067dc:
    // b63 @0x100067dc
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0x965e0878) == 0x1)) {
      // b282 @0x1000ba74
      t395 = var_19c8;
      t396 = var_1aa8;
      var_400 = var_19e4;
      t397 = var_19e0;
      var_3fc = t397;
      t398 = var_19dc;
      var_3f8 = t398;
      t399 = var_19ec;
      var_404 = t399;
      t400 = var_19d0;
      var_3f4 = t400;
      t401 = var_19cc;
      var_3f0 = t401;
      t402 = var_19d8;
      _cse994 = (t395 << 0x2);
      t403 = (*(uint32_t*)(((__entry_r125 - 0x1180) + _cse994)));
      _cse995 = ((int64_t)(int32_t)(var_19d4));
      t404 = (*(uint32_t*)(((((((_cse995 & 0xff) % 0x3) == var_1a98) == 0x1) ? 0x100002d0 : 0x100003d0) + _cse994)));
      var_7a4 = ((uint32_t)(_cse996));
      var_7a0 = ((uint32_t)(_cse999));
      _cse998 = ((int64_t)(int32_t)(var_19e4));
      _cse997 = ((((((((int64_t)(int32_t)(t398)) & (~(_cse998))) | (_cse998 & ((int64_t)(int32_t)(t397)))) + ((int64_t)(int32_t)(t402))) + ((int64_t)(int32_t)(t403))) + (((((_cse998 & var_1ac8) >> 0x7) | (_cse998 << 0x19)) ^ (((_cse998 & 0xffffe000) >> 0xd) | (_cse998 << 0x13))) ^ (((_cse998 & 0xfffe0000) >> 0x11) | (_cse998 << 0xf)))) + ((int64_t)(int32_t)(t404)));
      _cse996 = (_cse997 + ((int64_t)(int32_t)(var_19e8)));
      _cse1000 = ((int64_t)(int32_t)(t399));
      _cse1001 = ((int64_t)(int32_t)(t400));
      _cse1002 = ((int64_t)(int32_t)(t401));
      _cse999 = (((((_cse1000 ^ _cse1001) & _cse1002) ^ (_cse1000 & _cse1001)) + (((((_cse1002 & 0xffffffe0) >> 0x5) | (_cse1002 << 0x1b)) ^ (((_cse1002 & 0xfffff800) >> 0xb) | (_cse1002 << 0x15))) ^ (((_cse1002 & 0xfff80000) >> 0x13) | (_cse1002 << 0xd)))) + _cse997);
      _cse1003 = (t395 + 0x1);
      if (((_cse1003 == t396) == 0x1)) {
        var_1364 = 0xffffffffcf1cbe9d;
      } else {
        var_1364 = 0xffffffff965e0878;
      }
      var_19c8 = _cse1003;
      var_19cc = var_7a0;
      var_19d0 = t401;
      var_19d4 = ((uint32_t)(((_cse995 & 0xffffffff) + 0x1)));
      var_19d8 = t398;
      var_19dc = t397;
      var_19e0 = var_19e4;
      var_19e4 = var_7a4;
      var_19e8 = t399;
      var_19ec = t400;
      _cse1004 = (t395 + 0x1);
      t0 = _cse1004;
      _cse1005 = ((int64_t)(int32_t)(t399));
      _cse1006 = ((int64_t)(int32_t)(t400));
      _cse1007 = (_cse1005 & _cse1006);
      t1 = (((_cse1005 ^ _cse1006) & ((int64_t)(int32_t)(t401))) ^ _cse1007);
      if (((_cse1004 == t396) == 0x1)) {
        t2 = 0xffffffffcf1cbe9d;
      } else {
        t2 = 0xffffffff965e0878;
      }
      t3 = _cse1007;
      t4 = _cse1005;
      goto L_0x10004d40;
    }
    // b65 @0x100067f8
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xa253bf54) == 0x1)) {
      // b67 @0x10006814
      var_1278 = 0xdb410848330c48ff;
      var_1280 = 0x466acacfae6f138;
      var_1288 = 0x835bfa63d94cd18e;
      var_1290 = 0x98fda9b62327b145;
      var_1268 = 0x3b1192e82ce0259;
      var_1270 = 0xc8f879885708d847;
      var_1258 = 0xcb1e3f93f2ed0ea2;
      var_1260 = 0x3887a5c60f8c7976;
      var_1364 = 0x6e8cd62a;
    }
    goto L_0x10004d40;
L_0x100081a8:
    // b203 @0x100081a8
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xa2df69c5) == 0x1)) {
      // b314 @0x10011fd0
      var_4e8 = var_13ec;
      (*(uint8_t*)(((__entry_r125 - 0x1080) + (((int64_t)(int32_t)(var_13dc)) & 0xffffffff)))) = (*(uint8_t*)((var_fe0 + var_13e8)));
      t682 = (*var_f60);
      t683 = var_1aa8;
      _cse1009 = (((int64_t)(int32_t)(t682)) + 0x1);
      _cse1008 = ((uint32_t)(_cse1009));
      (*var_f60) = _cse1008;
      var_1428 = _cse1008;
      if ((((_cse1009 & 0xffffffff) == t683) == 0x1)) {
        var_1364 = 0xffffffffacc542b9;
      } else {
        var_1364 = 0x5384b4e3;
      }
      t0 = 0xffffffffacc542b9;
      goto L_0x10004d40;
    }
    // b205 @0x100081c4
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xa7375b0e) == 0x1)) {
      // b207 @0x100081e0
      var_1268 = 0x3b1192e82ce0259;
      var_1270 = 0xc8f879885708d847;
      var_1258 = 0xcb1e3f93f2ed0ea2;
      var_1260 = 0x3887a5c60f8c7976;
      var_1278 = 0xdb410848330c48ff;
      var_1280 = 0x466acacfae6f138;
      var_1288 = 0x835bfa63d94cd18e;
      var_1290 = 0x98fda9b62327b145;
      var_1364 = 0x6e8cd62a;
    }
    goto L_0x10004d40;
L_0x10004db0:
    // b9 @0x10004db0
    if ((((int64_t)(0xacc10971) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b173 @0x10007b04
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1b08) != 0x1)) {
        goto L_0x10007b1c;
      }
      // b305 @0x1000f52c
      t519 = var_13a0;
      t520 = var_1a98;
      t521 = var_1390;
      t522 = (*(uint8_t*)(t521));
      t523 = var_1398;
      (*(uint8_t*)(t523)) = t522;
      var_1390 = (t521 + 0x1);
      var_1398 = (t523 + 0x1);
      var_13a0 = (t519 - 0x1);
      if (((var_13a0 == t520) == 0x1)) {
        var_1364 = 0xffffffff89fd6da1;
      } else {
        var_1364 = 0xffffffffacc10972;
      }
      t0 = ((int64_t)(int8_t)(t522));
      t1 = t523;
      goto L_0x10004d40;
    }
    // b11 @0x10004dcc
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xa8246971) == 0x1)) {
      // b275 @0x10009cec
      (*var_f50) = ((uint32_t)((((int64_t)(int32_t)(var_4bc)) + ((int64_t)(int32_t)(var_c54)))));
      (*var_f48) = ((uint32_t)((((int64_t)(int32_t)(var_4c0)) + ((int64_t)(int32_t)(var_c50)))));
      (*var_f40) = ((uint32_t)((((int64_t)(int32_t)(var_4c4)) + ((int64_t)(int32_t)(var_c4c)))));
      (*var_f38) = ((uint32_t)((((int64_t)(int32_t)(var_c34)) + ((int64_t)(int32_t)(var_c48)))));
      (*var_f30) = ((uint32_t)((((int64_t)(int32_t)(var_4c8)) + ((int64_t)(int32_t)(var_c44)))));
      (*var_f28) = ((uint32_t)((((int64_t)(int32_t)(var_4b8)) + ((int64_t)(int32_t)(var_c40)))));
      (*var_f20) = ((uint32_t)((((int64_t)(int32_t)(var_4b4)) + ((int64_t)(int32_t)(var_c3c)))));
      (*var_f18) = ((uint32_t)((((int64_t)(int32_t)(var_c30)) + ((int64_t)(int32_t)(var_c38)))));
      /* 1 argument slot(s) hold values this function never wrote: either the callee reads a stale register or a definition was lost here */
      t299 = vm_memset(var_f68, 0x0, 0x38, 0x10016a2c, t1, t2, t3, t4);
      var_1364 = 0x19b80fb0;
      t300 = var_ef8;
      t301 = var_ef0;
      t302 = var_ee8;
      t303 = var_ee0;
      t304 = var_ed8;
      var_16f8 = var_c98;
      var_1700 = var_cc0;
      var_1708 = var_ce8;
      var_1710 = var_cf0;
      var_1718 = var_cf8;
      var_1720 = var_d00;
      var_1728 = var_d08;
      var_1730 = var_d10;
      var_1738 = var_d18;
      var_1740 = var_d20;
      var_1748 = var_d28;
      var_1750 = var_d30;
      var_1758 = var_d38;
      var_1760 = var_d40;
      var_1768 = var_d48;
      var_1770 = var_d50;
      var_1778 = var_d58;
      var_1780 = var_d60;
      var_1788 = var_d68;
      var_1790 = var_d70;
      var_1798 = var_d78;
      var_17a0 = var_d80;
      var_17a8 = var_d88;
      var_17b0 = var_d90;
      var_17b8 = var_d98;
      var_17c0 = var_da0;
      var_17c8 = var_da8;
      var_17d0 = var_db0;
      var_17d8 = var_db8;
      var_17e0 = var_dc0;
      var_17e8 = var_dc8;
      var_17f0 = var_dd0;
      var_17f8 = var_dd8;
      var_1800 = var_de0;
      var_1808 = var_de8;
      var_1810 = var_df0;
      var_1818 = var_df8;
      var_1820 = var_e00;
      var_1828 = var_e08;
      var_1830 = var_e10;
      var_1838 = var_e18;
      var_1840 = var_e20;
      var_1848 = var_e28;
      var_1850 = var_e30;
      var_1858 = var_e38;
      var_1860 = var_e40;
      var_1868 = var_e48;
      var_1870 = var_e50;
      var_1878 = var_e58;
      var_1880 = var_e60;
      var_1888 = var_e68;
      var_1890 = var_e70;
      var_1898 = var_e78;
      var_18a0 = var_e80;
      var_18a8 = var_e88;
      var_18b0 = var_e90;
      var_18b8 = var_e98;
      var_18c0 = var_ea0;
      var_18c8 = var_ea8;
      var_18d0 = var_eb0;
      var_18d8 = var_eb8;
      var_18e0 = var_ec0;
      var_18e8 = var_ec8;
      var_18f0 = var_ed0;
      var_18f8 = t304;
      var_1900 = t303;
      var_1908 = t302;
      var_1910 = t301;
      var_1918 = t300;
      var_1920 = var_f00;
      var_1928 = var_f08;
      var_1930 = var_f10;
      var_1938 = var_ce0;
      var_1940 = var_cc8;
      var_1948 = var_cd0;
      var_1950 = var_cd8;
      var_1958 = var_cb8;
      var_1960 = var_ca0;
      var_1968 = var_ca8;
      var_1970 = var_cb0;
      t0 = t300;
      t1 = t301;
      t2 = t302;
      t3 = t303;
      t4 = t304;
      goto L_0x10004d40;
    }
    // b13 @0x10004de8
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xa83e330a) == 0x1)) {
      // b276 @0x1000a5f4
      (*var_f50) = ((uint32_t)((((int64_t)(int32_t)(var_4d8)) + ((int64_t)(int32_t)(var_c8c)))));
      (*var_f48) = ((uint32_t)((((int64_t)(int32_t)(var_4dc)) + ((int64_t)(int32_t)(var_c88)))));
      (*var_f40) = ((uint32_t)((((int64_t)(int32_t)(var_4e0)) + ((int64_t)(int32_t)(var_c84)))));
      (*var_f38) = ((uint32_t)((((int64_t)(int32_t)(var_c6c)) + ((int64_t)(int32_t)(var_c80)))));
      (*var_f30) = ((uint32_t)((((int64_t)(int32_t)(var_4e4)) + ((int64_t)(int32_t)(var_c7c)))));
      (*var_f28) = ((uint32_t)((((int64_t)(int32_t)(var_4d4)) + ((int64_t)(int32_t)(var_c78)))));
      (*var_f20) = ((uint32_t)((((int64_t)(int32_t)(var_4d0)) + ((int64_t)(int32_t)(var_c74)))));
      (*var_f18) = ((uint32_t)((((int64_t)(int32_t)(var_c68)) + ((int64_t)(int32_t)(var_c70)))));
      (*var_f58) = ((*var_f58) + 0x200);
      t305 = var_1a98;
      _cse1010 = ((uint32_t)(t305));
      (*var_f60) = _cse1010;
      var_1428 = _cse1010;
      var_1364 = 0x5384b4e3;
      goto L_0x10004d40;
    }
    // b15 @0x10004e04
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xa87976bb) == 0x1)) {
      // b17 @0x10004e20
      t9 = var_fe0;
      (*var_fd0) = var_c02;
      t10 = var_fe0;
      (*(uint8_t*)((t9 + 0x41))) = var_b91;
      t11 = var_fe0;
      (*(uint8_t*)((t10 + 0x42))) = var_b11;
      t12 = var_fe0;
      (*(uint8_t*)((t11 + 0x43))) = var_a91;
      t13 = var_fe0;
      (*(uint8_t*)((t12 + 0x44))) = var_c01;
      t14 = var_fe0;
      (*(uint8_t*)((t13 + 0x45))) = var_b81;
      t15 = var_fe0;
      (*(uint8_t*)((t14 + 0x46))) = var_b01;
      t16 = var_fe0;
      (*(uint8_t*)((t15 + 0x47))) = var_a81;
      t17 = var_fe0;
      (*(uint8_t*)((t16 + 0x48))) = var_bf1;
      t18 = var_fe0;
      (*(uint8_t*)((t17 + 0x49))) = var_b71;
      t19 = var_fe0;
      (*(uint8_t*)((t18 + 0x4a))) = var_af1;
      t20 = var_fe0;
      (*(uint8_t*)((t19 + 0x4b))) = var_a71;
      t21 = var_fe0;
      (*(uint8_t*)((t20 + 0x4c))) = var_be1;
      t22 = var_fe0;
      (*(uint8_t*)((t21 + 0x4d))) = var_b61;
      t23 = var_fe0;
      (*(uint8_t*)((t22 + 0x4e))) = var_ae1;
      t24 = var_fe0;
      (*(uint8_t*)((t23 + 0x4f))) = var_a61;
      t25 = var_fe0;
      (*(uint8_t*)((t24 + 0x50))) = var_bd1;
      t26 = var_fe0;
      (*(uint8_t*)((t25 + 0x51))) = var_b51;
      t27 = var_fe0;
      (*(uint8_t*)((t26 + 0x52))) = var_ad1;
      t28 = var_fe0;
      (*(uint8_t*)((t27 + 0x53))) = var_a51;
      t29 = var_fe0;
      (*(uint8_t*)((t28 + 0x54))) = var_bc1;
      t30 = var_fe0;
      (*(uint8_t*)((t29 + 0x55))) = var_b41;
      t31 = var_fe0;
      (*(uint8_t*)((t30 + 0x56))) = var_ac1;
      t32 = var_fe0;
      (*(uint8_t*)((t31 + 0x57))) = var_a41;
      t33 = var_fe0;
      (*(uint8_t*)((t32 + 0x58))) = var_bb1;
      t34 = var_fe0;
      (*(uint8_t*)((t33 + 0x59))) = var_b31;
      t35 = var_fe0;
      (*(uint8_t*)((t34 + 0x5a))) = var_ab1;
      t36 = var_fe0;
      (*(uint8_t*)((t35 + 0x5b))) = var_a31;
      t37 = var_fe0;
      (*(uint8_t*)((t36 + 0x5c))) = var_ba1;
      t38 = var_fe0;
      (*(uint8_t*)((t37 + 0x5d))) = var_b21;
      t39 = var_fe0;
      (*(uint8_t*)((t38 + 0x5e))) = var_aa1;
      (*(uint8_t*)((t39 + 0x5f))) = var_a21;
      var_1364 = 0xb8d08280;
    }
    goto L_0x10004d40;
L_0x10007b1c:
    // b175 @0x10007b1c
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xacc542b9) == 0x1)) {
      // b306 @0x1000f5bc
      (*var_ef0) = ((uint32_t)((((((((int64_t)(int8_t)((*var_f08))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_f68))) << 0x18)) | ((((int64_t)(int8_t)((*var_f00))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_ef8))) & 0xff))));
      (*var_ec8) = ((uint32_t)((((((((int64_t)(int8_t)((*var_ee0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_ee8))) << 0x18)) | ((((int64_t)(int8_t)((*var_ed8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_ed0))) & 0xff))));
      (*var_ea0) = ((uint32_t)((((((((int64_t)(int8_t)((*var_eb8))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_ec0))) << 0x18)) | ((((int64_t)(int8_t)((*var_eb0))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_ea8))) & 0xff))));
      (*var_e78) = ((uint32_t)((((((((int64_t)(int8_t)((*var_e90))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_e98))) << 0x18)) | ((((int64_t)(int8_t)((*var_e88))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_e80))) & 0xff))));
      (*var_e50) = ((uint32_t)((((((((int64_t)(int8_t)((*var_e68))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_e70))) << 0x18)) | ((((int64_t)(int8_t)((*var_e60))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_e58))) & 0xff))));
      (*var_e28) = ((uint32_t)((((((((int64_t)(int8_t)((*var_e40))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_e48))) << 0x18)) | ((((int64_t)(int8_t)((*var_e38))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_e30))) & 0xff))));
      (*var_e00) = ((uint32_t)((((((((int64_t)(int8_t)((*var_e18))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_e20))) << 0x18)) | ((((int64_t)(int8_t)((*var_e10))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_e08))) & 0xff))));
      (*var_dd8) = ((uint32_t)((((((((int64_t)(int8_t)((*var_df0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_df8))) << 0x18)) | ((((int64_t)(int8_t)((*var_de8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_de0))) & 0xff))));
      (*var_db0) = ((uint32_t)((((((((int64_t)(int8_t)((*var_dc8))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_dd0))) << 0x18)) | ((((int64_t)(int8_t)((*var_dc0))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_db8))) & 0xff))));
      (*var_d88) = ((uint32_t)((((((((int64_t)(int8_t)((*var_da0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_da8))) << 0x18)) | ((((int64_t)(int8_t)((*var_d98))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_d90))) & 0xff))));
      (*var_d60) = ((uint32_t)((((((((int64_t)(int8_t)((*var_d78))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_d80))) << 0x18)) | ((((int64_t)(int8_t)((*var_d70))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_d68))) & 0xff))));
      (*var_d38) = ((uint32_t)((((((((int64_t)(int8_t)((*var_d50))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_d58))) << 0x18)) | ((((int64_t)(int8_t)((*var_d48))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_d40))) & 0xff))));
      (*var_d10) = ((uint32_t)((((((((int64_t)(int8_t)((*var_d28))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_d30))) << 0x18)) | ((((int64_t)(int8_t)((*var_d20))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_d18))) & 0xff))));
      (*var_ce8) = ((uint32_t)((((((((int64_t)(int8_t)((*var_d00))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_d08))) << 0x18)) | ((((int64_t)(int8_t)((*var_cf8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_cf0))) & 0xff))));
      (*var_cc0) = ((uint32_t)((((((((int64_t)(int8_t)((*var_cd8))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_ce0))) << 0x18)) | ((((int64_t)(int8_t)((*var_cd0))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_cc8))) & 0xff))));
      (*var_c98) = ((uint32_t)((((((((int64_t)(int8_t)((*var_cb0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_cb8))) << 0x18)) | ((((int64_t)(int8_t)((*var_ca8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_ca0))) & 0xff))));
      var_13f8 = var_1b10;
      var_1364 = 0xdff5c815;
      goto L_0x10004d40;
    }
    // b177 @0x10007b38
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1b60) == 0x1)) {
      // b179 @0x10007b50
      var_1a7c = ((uint32_t)(var_1cf8));
      if ((((uint64_t)(var_1d10) == var_1a98) == 0x1)) {
        var_1364 = var_1ac0;
      } else {
        var_1364 = var_1b58;
      }
    }
    goto L_0x10004d40;
L_0x10007164:
    // b105 @0x10007164
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xb8d08280) == 0x1)) {
      // b290 @0x1000c318
      var_a10 = (uint32_t*)&var_11b0;
      _cse1011 = (__entry_r125 - 0x11f0);
      var_a18 = (uint8_t*)_cse1011;
      t427 = var_1a98;
      var_11b0 = ((uint32_t)(t427));
      var_a08 = (uint64_t*)&var_11a8;
      var_11a8 = t427;
      var_a00 = (uint32_t*)&var_11a0;
      var_9f8 = (uint32_t*)(__entry_r125 - 0x119c);
      var_9f0 = (uint32_t*)&var_1198;
      var_9e8 = (uint32_t*)(__entry_r125 - 0x1194);
      var_1198 = 0x5f96bc2640fcca21;
      var_11a0 = 0x55c746b4a195712e;
      var_9e0 = (uint32_t*)&var_1190;
      var_9d8 = (uint32_t*)(__entry_r125 - 0x118c);
      var_9d0 = (uint32_t*)&var_1188;
      var_9c8 = (uint32_t*)(__entry_r125 - 0x1184);
      var_1188 = 0x52a98cf79b113ee0;
      var_1190 = 0xc54dc1c7d18a047b;
      var_9b8 = (uint8_t*)(_cse1011 | 0x2);
      var_9c0 = (uint8_t*)(_cse1011 | 0x3);
      var_9b0 = (uint8_t*)(_cse1011 | 0x1);
      var_9a8 = (uint8_t*)(_cse1011 | 0x4);
      var_9a0 = (uint8_t*)(_cse1011 | 0x7);
      var_998 = (uint8_t*)(_cse1011 | 0x6);
      var_990 = (uint8_t*)(_cse1011 | 0x5);
      var_988 = (uint8_t*)(__entry_r125 - 0x11e8);
      var_980 = (uint8_t*)(__entry_r125 - 0x11e5);
      var_978 = (uint8_t*)(__entry_r125 - 0x11e6);
      var_970 = (uint8_t*)(__entry_r125 - 0x11e7);
      var_968 = (uint8_t*)(__entry_r125 - 0x11e4);
      var_960 = (uint8_t*)(__entry_r125 - 0x11e1);
      var_958 = (uint8_t*)(__entry_r125 - 0x11e2);
      var_950 = (uint8_t*)(__entry_r125 - 0x11e3);
      var_948 = (uint8_t*)(__entry_r125 - 0x11e0);
      var_940 = (uint8_t*)(__entry_r125 - 0x11dd);
      var_938 = (uint8_t*)(__entry_r125 - 0x11de);
      var_930 = (uint8_t*)(__entry_r125 - 0x11df);
      var_920 = (uint8_t*)(__entry_r125 - 0x11d9);
      var_928 = (uint8_t*)(__entry_r125 - 0x11dc);
      var_918 = (uint8_t*)(__entry_r125 - 0x11da);
      var_910 = (uint8_t*)(__entry_r125 - 0x11db);
      var_908 = (uint8_t*)(__entry_r125 - 0x11d8);
      var_900 = (uint8_t*)(__entry_r125 - 0x11d5);
      var_8f8 = (uint8_t*)(__entry_r125 - 0x11d6);
      var_8f0 = (uint8_t*)(__entry_r125 - 0x11d7);
      var_8e8 = (uint8_t*)(__entry_r125 - 0x11d4);
      var_8e0 = (uint8_t*)(__entry_r125 - 0x11d1);
      var_8d8 = (uint8_t*)(__entry_r125 - 0x11d2);
      var_8d0 = (uint8_t*)(__entry_r125 - 0x11d3);
      var_8c8 = (uint8_t*)(__entry_r125 - 0x11d0);
      var_8c0 = (uint8_t*)(__entry_r125 - 0x11cd);
      var_8b8 = (uint8_t*)(__entry_r125 - 0x11ce);
      var_8b0 = (uint8_t*)(__entry_r125 - 0x11cf);
      var_8a8 = (uint8_t*)(__entry_r125 - 0x11cc);
      var_8a0 = (uint8_t*)(__entry_r125 - 0x11c9);
      var_898 = (uint8_t*)(__entry_r125 - 0x11ca);
      var_888 = (uint8_t*)(__entry_r125 - 0x11c8);
      var_890 = (uint8_t*)(__entry_r125 - 0x11cb);
      var_880 = (uint8_t*)(__entry_r125 - 0x11c5);
      var_878 = (uint8_t*)(__entry_r125 - 0x11c6);
      var_870 = (uint8_t*)(__entry_r125 - 0x11c7);
      var_868 = (uint8_t*)(__entry_r125 - 0x11c4);
      var_860 = (uint8_t*)(__entry_r125 - 0x11c1);
      var_858 = (uint8_t*)(__entry_r125 - 0x11c2);
      var_850 = (uint8_t*)(__entry_r125 - 0x11c3);
      var_848 = (uint8_t*)(__entry_r125 - 0x11c0);
      var_840 = (uint8_t*)(__entry_r125 - 0x11bd);
      var_838 = (uint8_t*)(__entry_r125 - 0x11be);
      var_830 = (uint8_t*)(__entry_r125 - 0x11bf);
      var_828 = (uint8_t*)(__entry_r125 - 0x11bc);
      var_820 = (uint8_t*)(__entry_r125 - 0x11b9);
      var_818 = (uint8_t*)(__entry_r125 - 0x11ba);
      var_810 = (uint8_t*)(__entry_r125 - 0x11bb);
      var_808 = (uint8_t*)(__entry_r125 - 0x11b8);
      var_800 = (uint8_t*)(__entry_r125 - 0x11b5);
      var_7f8 = (uint8_t*)(__entry_r125 - 0x11b6);
      var_7f0 = (uint8_t*)(__entry_r125 - 0x11b7);
      var_7e8 = (uint8_t*)(__entry_r125 - 0x11b4);
      var_7e0 = (uint8_t*)(__entry_r125 - 0x11b1);
      var_7d8 = (uint8_t*)(__entry_r125 - 0x11b2);
      var_7d0 = (uint8_t*)(__entry_r125 - 0x11b3);
      var_1364 = ((uint32_t)(var_1af8));
      var_19b0 = t427;
      var_19b4 = var_11b0;
      t0 = t427;
      goto L_0x10004d40;
    }
    // b107 @0x10007180
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xbb17d68b) == 0x1)) {
      // b109 @0x1000719c
      if (((var_1ad0 == var_1a98) == 0x1)) {
        var_1364 = 0xffffffff89fd6da1;
      } else {
        var_1364 = 0xfffffffffad73dca;
      }
    }
    goto L_0x10004d40;
L_0x10008e6c:
    // b231 @0x10008e6c
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xbd960277) == 0x1)) {
      // b322 @0x10014b10
      (*var_4a8) = ((uint32_t)((((((((int64_t)(int8_t)((*var_9c0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_a18))) << 0x18)) | ((((int64_t)(int8_t)((*var_9b8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_9b0))) & 0xff))));
      (*var_4a0) = ((uint32_t)((((((((int64_t)(int8_t)((*var_9a0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_9a8))) << 0x18)) | ((((int64_t)(int8_t)((*var_998))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_990))) & 0xff))));
      (*var_498) = ((uint32_t)((((((((int64_t)(int8_t)((*var_980))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_988))) << 0x18)) | ((((int64_t)(int8_t)((*var_978))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_970))) & 0xff))));
      (*var_490) = ((uint32_t)((((((((int64_t)(int8_t)((*var_960))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_968))) << 0x18)) | ((((int64_t)(int8_t)((*var_958))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_950))) & 0xff))));
      (*var_488) = ((uint32_t)((((((((int64_t)(int8_t)((*var_940))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_948))) << 0x18)) | ((((int64_t)(int8_t)((*var_938))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_930))) & 0xff))));
      (*var_480) = ((uint32_t)((((((((int64_t)(int8_t)((*var_920))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_928))) << 0x18)) | ((((int64_t)(int8_t)((*var_918))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_910))) & 0xff))));
      (*var_478) = ((uint32_t)((((((((int64_t)(int8_t)((*var_900))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_908))) << 0x18)) | ((((int64_t)(int8_t)((*var_8f8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_8f0))) & 0xff))));
      (*var_470) = ((uint32_t)((((((((int64_t)(int8_t)((*var_8e0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_8e8))) << 0x18)) | ((((int64_t)(int8_t)((*var_8d8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_8d0))) & 0xff))));
      (*var_468) = ((uint32_t)((((((((int64_t)(int8_t)((*var_8c0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_8c8))) << 0x18)) | ((((int64_t)(int8_t)((*var_8b8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_8b0))) & 0xff))));
      (*var_460) = ((uint32_t)((((((((int64_t)(int8_t)((*var_8a0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_8a8))) << 0x18)) | ((((int64_t)(int8_t)((*var_898))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_890))) & 0xff))));
      (*var_458) = ((uint32_t)((((((((int64_t)(int8_t)((*var_880))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_888))) << 0x18)) | ((((int64_t)(int8_t)((*var_878))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_870))) & 0xff))));
      (*var_450) = ((uint32_t)((((((((int64_t)(int8_t)((*var_860))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_868))) << 0x18)) | ((((int64_t)(int8_t)((*var_858))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_850))) & 0xff))));
      (*var_448) = ((uint32_t)((((((((int64_t)(int8_t)((*var_840))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_848))) << 0x18)) | ((((int64_t)(int8_t)((*var_838))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_830))) & 0xff))));
      (*var_440) = ((uint32_t)((((((((int64_t)(int8_t)((*var_820))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_828))) << 0x18)) | ((((int64_t)(int8_t)((*var_818))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_810))) & 0xff))));
      (*var_438) = ((uint32_t)((((((((int64_t)(int8_t)((*var_800))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_808))) << 0x18)) | ((((int64_t)(int8_t)((*var_7f8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_7f0))) & 0xff))));
      (*var_430) = ((uint32_t)((((((((int64_t)(int8_t)((*var_7e0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_7e8))) << 0x18)) | ((((int64_t)(int8_t)((*var_7d8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_7d0))) & 0xff))));
      var_19c0 = var_1b10;
      var_1364 = 0x2d207b9c;
      goto L_0x10004d40;
    }
    // b233 @0x10008e88
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xc0a982cf) == 0x1)) {
      // b235 @0x10008ea4
      (*var_4a8) = ((uint32_t)((((((((int64_t)(int8_t)((*var_9c0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_a18))) << 0x18)) | ((((int64_t)(int8_t)((*var_9b8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_9b0))) & 0xff))));
      (*var_4a0) = ((uint32_t)((((((((int64_t)(int8_t)((*var_9a0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_9a8))) << 0x18)) | ((((int64_t)(int8_t)((*var_998))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_990))) & 0xff))));
      (*var_498) = ((uint32_t)((((((((int64_t)(int8_t)((*var_980))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_988))) << 0x18)) | ((((int64_t)(int8_t)((*var_978))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_970))) & 0xff))));
      (*var_490) = ((uint32_t)((((((((int64_t)(int8_t)((*var_960))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_968))) << 0x18)) | ((((int64_t)(int8_t)((*var_958))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_950))) & 0xff))));
      (*var_488) = ((uint32_t)((((((((int64_t)(int8_t)((*var_940))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_948))) << 0x18)) | ((((int64_t)(int8_t)((*var_938))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_930))) & 0xff))));
      (*var_480) = ((uint32_t)((((((((int64_t)(int8_t)((*var_920))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_928))) << 0x18)) | ((((int64_t)(int8_t)((*var_918))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_910))) & 0xff))));
      (*var_478) = ((uint32_t)((((((((int64_t)(int8_t)((*var_900))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_908))) << 0x18)) | ((((int64_t)(int8_t)((*var_8f8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_8f0))) & 0xff))));
      (*var_470) = ((uint32_t)((((((((int64_t)(int8_t)((*var_8e0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_8e8))) << 0x18)) | ((((int64_t)(int8_t)((*var_8d8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_8d0))) & 0xff))));
      (*var_468) = ((uint32_t)((((((((int64_t)(int8_t)((*var_8c0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_8c8))) << 0x18)) | ((((int64_t)(int8_t)((*var_8b8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_8b0))) & 0xff))));
      (*var_460) = ((uint32_t)((((((((int64_t)(int8_t)((*var_8a0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_8a8))) << 0x18)) | ((((int64_t)(int8_t)((*var_898))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_890))) & 0xff))));
      (*var_458) = ((uint32_t)((((((((int64_t)(int8_t)((*var_880))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_888))) << 0x18)) | ((((int64_t)(int8_t)((*var_878))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_870))) & 0xff))));
      (*var_450) = ((uint32_t)((((((((int64_t)(int8_t)((*var_860))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_868))) << 0x18)) | ((((int64_t)(int8_t)((*var_858))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_850))) & 0xff))));
      (*var_448) = ((uint32_t)((((((((int64_t)(int8_t)((*var_840))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_848))) << 0x18)) | ((((int64_t)(int8_t)((*var_838))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_830))) & 0xff))));
      (*var_440) = ((uint32_t)((((((((int64_t)(int8_t)((*var_820))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_828))) << 0x18)) | ((((int64_t)(int8_t)((*var_818))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_810))) & 0xff))));
      (*var_438) = ((uint32_t)((((((((int64_t)(int8_t)((*var_800))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_808))) << 0x18)) | ((((int64_t)(int8_t)((*var_7f8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_7f0))) & 0xff))));
      (*var_430) = ((uint32_t)((((((((int64_t)(int8_t)((*var_7e0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_7e8))) << 0x18)) | ((((int64_t)(int8_t)((*var_7d8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_7d0))) & 0xff))));
      var_19f8 = var_1b10;
      var_1364 = 0xcaa685fb;
    }
    goto L_0x10004d40;
L_0x10007948:
    // b155 @0x10007948
    if ((((int64_t)(0xc5891d48) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b157 @0x10007964
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xc5891d49) != 0x1)) {
        goto L_0x10007980;
      }
      // b301 @0x1000ea3c
      t489 = var_1298;
      t490 = var_fe0;
      (*(uint64_t*)((t490 + 0x30))) = var_12a0;
      (*(uint64_t*)((t490 + 0x38))) = t489;
      t491 = var_12b0;
      t492 = var_fe0;
      (*(uint64_t*)((t492 + 0x28))) = var_12a8;
      (*(uint64_t*)((t492 + 0x20))) = t491;
      t493 = var_12c0;
      t494 = var_fe0;
      (*(uint64_t*)((t494 + 0x18))) = var_12b8;
      (*(uint64_t*)((t494 + 0x10))) = t493;
      t495 = var_12d0;
      t496 = var_fe0;
      (*(uint64_t*)((t496 + 0x8))) = var_12c8;
      (*(uint64_t*)(t496)) = t495;
      var_1364 = 0x169b2590;
      goto L_0x10004d40;
    }
    // b265 @0x10009a8c
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xc2e9a270) == 0x1)) {
      // b330 @0x100161bc
      /* 1 argument slot(s) hold values this function never wrote: either the callee reads a stale register or a definition was lost here */
      t922 = vm_memset(((__entry_r125 - 0x1250) + var_500), 0x0, (0x3f - var_508), 0x10016a2c, t1, t2, t3, t4);
      var_1364 = 0x1ff94f5f;
      t0 = t1 = t2 = t3 = t4 = 0x0; /* t0, t1, t2, t3, t4: never set up on this edge */
      goto L_0x10004d40;
    }
    // b267 @0x10009aa8
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == var_1b50) == 0x1)) {
      // b269 @0x10009ac0
      t294 = var_1a30;
      t295 = var_1aa8;
      _cse1012 = (__entry_r125 - 0x1180);
      t296 = (*(uint32_t*)((_cse1012 + (((t294 + var_1b18) & 0xffffffff) << 0x2))));
      t297 = (*(uint32_t*)((_cse1012 + (((t294 + 0xfffffff9) & 0xffffffff) << 0x2))));
      t298 = (*(uint32_t*)((_cse1012 + (((t294 + var_1ad8) & 0xffffffff) << 0x2))));
      _cse1013 = (((int64_t)(int32_t)((*(uint32_t*)((_cse1012 + (((t294 + 0xfffffffe) & 0xffffffff) << 0x2)))))) & 0xffffffff);
      _cse1014 = (((int64_t)(int32_t)(t296)) & 0xffffffff);
      (*(uint32_t*)((_cse1012 + (t294 << 0x2)))) = ((uint32_t)((((((int64_t)(int32_t)(t298)) + ((int64_t)(int32_t)(t297))) + ((((_cse1013 >> 0x5) | (_cse1013 << 0x1b)) ^ (_cse1013 >> 0x2)) ^ ((_cse1013 >> 0x9) | (_cse1013 << 0x17)))) + ((((_cse1014 >> 0xb) | (_cse1014 << 0x15)) ^ (_cse1014 >> 0x5)) ^ ((_cse1014 >> 0x3) | (_cse1014 << 0x1d))))));
      var_1a30 = (t294 + 0x1);
      if (((var_1a30 == t295) == 0x1)) {
        var_1364 = 0xffffffff8a19f97e;
      } else {
        var_1364 = 0xffffffffc3706155;
      }
      if ((((t294 + 0x1) == t295) == 0x1)) {
        t0 = 0xffffffff8a19f97e;
      } else {
        t0 = 0xffffffffc3706155;
      }
      _cse1015 = ((int64_t)(int32_t)(t298));
      t1 = (_cse1015 + ((int64_t)(int32_t)(t297)));
      _cse1016 = (((int64_t)(int32_t)(t296)) & 0xffffffff);
      t2 = ((((_cse1016 >> 0xb) | (_cse1016 << 0x15)) ^ (_cse1016 >> 0x5)) ^ ((_cse1016 >> 0x3) | (_cse1016 << 0x1d)));
      t3 = _cse1015;
      t4 = (((t294 + 0xfffffff9) & 0xffffffff) << 0x2);
    }
    goto L_0x10004d40;
L_0x10007980:
    // b159 @0x10007980
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xc921fa00) == 0x1)) {
      // b302 @0x1000eafc
      t497 = (*var_a00);
      var_790 = t497;
      t498 = (*var_9f8);
      var_78c = t498;
      t499 = (*var_9f0);
      var_788 = t499;
      t500 = (*var_9e8);
      var_784 = t500;
      t501 = (*var_9e0);
      var_780 = t501;
      t502 = (*var_9d8);
      var_77c = t502;
      t503 = (*var_9d0);
      var_778 = t503;
      t504 = (*var_9c8);
      var_774 = t504;
      var_1364 = 0x3dc7793b;
      var_1a00 = var_1a98;
      var_1a0c = ((uint32_t)(var_1a98));
      var_1a04 = t497;
      var_1a08 = t498;
      var_1a20 = t500;
      var_1a1c = t501;
      var_1a18 = t502;
      var_1a14 = t503;
      var_1a10 = t504;
      var_1a24 = t499;
      goto L_0x10004d40;
    }
    // b161 @0x1000799c
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xc94e0219) == 0x1)) {
      // b163 @0x100079b8
      /* 1 argument slot(s) hold values this function never wrote: either the callee reads a stale register or a definition was lost here */
      t184 = vm_memset((((__entry_r125 - 0x11f0) + var_798) + 0x1), 0x0, (((0x36 - ((int64_t)(int32_t)(var_3ec))) & 0xffffffff) + 0x1), 0x10016a2c, t1, t2, t3, t4);
      var_1364 = ((uint32_t)(var_1ae8));
      t0 = t1 = t2 = t3 = t4 = 0x0; /* t0, t1, t2, t3, t4: never set up on this edge */
    }
    goto L_0x10004d40;
L_0x10006a90:
    // b85 @0x10006a90
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xcd7988c6) == 0x1)) {
      // b286 @0x1000c058
      t416 = var_410;
      t417 = var_1af8;
      var_3ec = var_19f0;
      var_19b0 = (t416 + 0x1);
      var_19b4 = var_19f0;
      if (((var_19b0 == 0x60) == 0x1)) {
        var_1364 = 0x6912374d;
      } else {
        var_1364 = t417;
      }
      if ((((t416 + 0x1) == 0x60) == 0x1)) {
        t0 = 0x6912374d;
      } else {
        t0 = t417;
      }
      goto L_0x10004d40;
    }
    // b87 @0x10006aac
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xcf0494cd) == 0x1)) {
      // b89 @0x10006ac8
      _cse1017 = (__entry_r125 - 0x1080);
      var_f08 = (uint8_t*)(_cse1017 | 0x3);
      var_f00 = (uint8_t*)(_cse1017 | 0x2);
      var_ef8 = (uint8_t*)(_cse1017 | 0x1);
      var_ee8 = (uint8_t*)(_cse1017 | 0x4);
      var_ee0 = (uint8_t*)(_cse1017 | 0x7);
      var_ed8 = (uint8_t*)(_cse1017 | 0x6);
      var_ed0 = (uint8_t*)(_cse1017 | 0x5);
      var_ec8 = (uint32_t*)(__entry_r125 - 0x117c);
      var_ec0 = (uint8_t*)(__entry_r125 - 0x1078);
      var_eb8 = (uint8_t*)(__entry_r125 - 0x1075);
      var_eb0 = (uint8_t*)(__entry_r125 - 0x1076);
      var_ea8 = (uint8_t*)(__entry_r125 - 0x1077);
      var_ea0 = (uint32_t*)(__entry_r125 - 0x1178);
      var_e98 = (uint8_t*)(__entry_r125 - 0x1074);
      var_e90 = (uint8_t*)(__entry_r125 - 0x1071);
      var_e88 = (uint8_t*)(__entry_r125 - 0x1072);
      var_e80 = (uint8_t*)(__entry_r125 - 0x1073);
      var_f10 = (__entry_r125 - 0x1180);
      var_ef0 = (uint32_t*)var_f10;
      var_e70 = (uint8_t*)(__entry_r125 - 0x1070);
      var_e78 = (uint32_t*)(__entry_r125 - 0x1174);
      var_e68 = (uint8_t*)(__entry_r125 - 0x106d);
      var_e60 = (uint8_t*)(__entry_r125 - 0x106e);
      var_e58 = (uint8_t*)(__entry_r125 - 0x106f);
      var_e50 = (uint32_t*)(__entry_r125 - 0x1170);
      var_e48 = (uint8_t*)(__entry_r125 - 0x106c);
      var_e40 = (uint8_t*)(__entry_r125 - 0x1069);
      var_e38 = (uint8_t*)(__entry_r125 - 0x106a);
      var_e30 = (uint8_t*)(__entry_r125 - 0x106b);
      var_e28 = (uint32_t*)(__entry_r125 - 0x116c);
      var_e20 = (uint8_t*)(__entry_r125 - 0x1068);
      var_e18 = (uint8_t*)(__entry_r125 - 0x1065);
      var_e10 = (uint8_t*)(__entry_r125 - 0x1066);
      var_e08 = (uint8_t*)(__entry_r125 - 0x1067);
      var_e00 = (uint32_t*)(__entry_r125 - 0x1168);
      var_df8 = (uint8_t*)(__entry_r125 - 0x1064);
      var_df0 = (uint8_t*)(__entry_r125 - 0x1061);
      var_de8 = (uint8_t*)(__entry_r125 - 0x1062);
      var_dd8 = (uint32_t*)(__entry_r125 - 0x1164);
      var_de0 = (uint8_t*)(__entry_r125 - 0x1063);
      var_dd0 = (uint8_t*)(__entry_r125 - 0x1060);
      var_dc8 = (uint8_t*)(__entry_r125 - 0x105d);
      var_dc0 = (uint8_t*)(__entry_r125 - 0x105e);
      var_db8 = (uint8_t*)(__entry_r125 - 0x105f);
      var_db0 = (uint32_t*)(__entry_r125 - 0x1160);
      var_da8 = (uint8_t*)(__entry_r125 - 0x105c);
      var_da0 = (uint8_t*)(__entry_r125 - 0x1059);
      var_d98 = (uint8_t*)(__entry_r125 - 0x105a);
      var_d90 = (uint8_t*)(__entry_r125 - 0x105b);
      var_d88 = (uint32_t*)(__entry_r125 - 0x115c);
      var_d80 = (uint8_t*)(__entry_r125 - 0x1058);
      var_d78 = (uint8_t*)(__entry_r125 - 0x1055);
      var_d70 = (uint8_t*)(__entry_r125 - 0x1056);
      var_d68 = (uint8_t*)(__entry_r125 - 0x1057);
      var_d60 = (uint32_t*)(__entry_r125 - 0x1158);
      var_d58 = (uint8_t*)(__entry_r125 - 0x1054);
      var_d50 = (uint8_t*)(__entry_r125 - 0x1051);
      var_d48 = (uint8_t*)(__entry_r125 - 0x1052);
      var_d40 = (uint8_t*)(__entry_r125 - 0x1053);
      var_d38 = (uint32_t*)(__entry_r125 - 0x1154);
      var_d30 = (uint8_t*)(__entry_r125 - 0x1050);
      var_d28 = (uint8_t*)(__entry_r125 - 0x104d);
      var_d20 = (uint8_t*)(__entry_r125 - 0x104e);
      var_d18 = (uint8_t*)(__entry_r125 - 0x104f);
      var_d10 = (uint32_t*)(__entry_r125 - 0x1150);
      var_d08 = (uint8_t*)(__entry_r125 - 0x104c);
      var_d00 = (uint8_t*)(__entry_r125 - 0x1049);
      var_cf8 = (uint8_t*)(__entry_r125 - 0x104a);
      var_cf0 = (uint8_t*)(__entry_r125 - 0x104b);
      var_ce8 = (uint32_t*)(__entry_r125 - 0x114c);
      var_ce0 = (uint8_t*)(__entry_r125 - 0x1048);
      var_cd8 = (uint8_t*)(__entry_r125 - 0x1045);
      var_cd0 = (uint8_t*)(__entry_r125 - 0x1046);
      var_cc8 = (uint8_t*)(__entry_r125 - 0x1047);
      var_cc0 = (uint32_t*)(__entry_r125 - 0x1148);
      var_cb8 = (uint8_t*)(__entry_r125 - 0x1044);
      var_cb0 = (uint8_t*)(__entry_r125 - 0x1041);
      var_ca8 = (uint8_t*)(__entry_r125 - 0x1042);
      var_ca0 = (uint8_t*)(__entry_r125 - 0x1043);
      var_c98 = (uint32_t*)(__entry_r125 - 0x1144);
      var_1364 = 0xa2df69c5;
      var_13dc = ((uint32_t)(var_1a98));
      var_13e8 = var_1a98;
      var_13ec = var_13dc;
    }
    goto L_0x10004d40;
L_0x10008c24:
    // b217 @0x10008c24
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xd170b27a) == 0x1)) {
      // b318 @0x100134b8
      t806 = var_768;
      t807 = var_3c4;
      t808 = var_764;
      t809 = var_3c8;
      t810 = var_760;
      t811 = var_3cc;
      t812 = var_75c;
      t813 = var_748;
      t814 = var_758;
      t815 = var_3d0;
      t816 = var_754;
      t817 = var_3c0;
      t818 = var_750;
      t819 = var_3bc;
      t820 = var_74c;
      t821 = var_744;
      _cse1018 = (((int64_t)(int32_t)(t807)) + ((int64_t)(int32_t)(t806)));
      (*var_fd8) = ((uint8_t)((_cse1018 >> 0x18)));
      _cse1019 = (((int64_t)(int32_t)(t809)) + ((int64_t)(int32_t)(t808)));
      (*var_c00) = ((uint8_t)((_cse1019 >> 0x18)));
      _cse1020 = (((int64_t)(int32_t)(t811)) + ((int64_t)(int32_t)(t810)));
      (*var_bf0) = ((uint8_t)((_cse1020 >> 0x18)));
      _cse1021 = (((int64_t)(int32_t)(t813)) + ((int64_t)(int32_t)(t812)));
      (*var_be0) = ((uint8_t)((_cse1021 >> 0x18)));
      _cse1022 = (((int64_t)(int32_t)(t815)) + ((int64_t)(int32_t)(t814)));
      (*var_bd0) = ((uint8_t)((_cse1022 >> 0x18)));
      _cse1023 = (((int64_t)(int32_t)(t817)) + ((int64_t)(int32_t)(t816)));
      (*var_bc0) = ((uint8_t)((_cse1023 >> 0x18)));
      _cse1024 = (((int64_t)(int32_t)(t819)) + ((int64_t)(int32_t)(t818)));
      (*var_bb0) = ((uint8_t)((_cse1024 >> 0x18)));
      _cse1025 = (((int64_t)(int32_t)(t821)) + ((int64_t)(int32_t)(t820)));
      (*var_ba0) = ((uint8_t)((_cse1025 >> 0x18)));
      (*var_b90) = ((uint8_t)((_cse1018 >> 0x10)));
      (*var_b80) = ((uint8_t)((_cse1019 >> 0x10)));
      (*var_b70) = ((uint8_t)((_cse1020 >> 0x10)));
      (*var_b60) = ((uint8_t)((_cse1021 >> 0x10)));
      (*var_b50) = ((uint8_t)((_cse1022 >> 0x10)));
      (*var_b40) = ((uint8_t)((_cse1023 >> 0x10)));
      (*var_b30) = ((uint8_t)((_cse1024 >> 0x10)));
      (*var_b20) = ((uint8_t)((_cse1025 >> 0x10)));
      (*var_b10) = ((uint8_t)((_cse1018 >> 0x8)));
      (*var_b00) = ((uint8_t)((_cse1019 >> 0x8)));
      (*var_af0) = ((uint8_t)((_cse1020 >> 0x8)));
      (*var_ae0) = ((uint8_t)((_cse1021 >> 0x8)));
      (*var_ad0) = ((uint8_t)((_cse1022 >> 0x8)));
      (*var_ac0) = ((uint8_t)((_cse1023 >> 0x8)));
      (*var_ab0) = ((uint8_t)((_cse1024 >> 0x8)));
      (*var_aa0) = ((uint8_t)((_cse1025 >> 0x8)));
      (*var_a90) = ((uint8_t)(_cse1018));
      (*var_a80) = ((uint8_t)(_cse1019));
      (*var_a70) = ((uint8_t)(_cse1020));
      (*var_a60) = ((uint8_t)(_cse1021));
      (*var_a50) = ((uint8_t)(_cse1022));
      (*var_a40) = ((uint8_t)(_cse1023));
      (*var_a30) = ((uint8_t)(_cse1024));
      (*var_a20) = ((uint8_t)(_cse1025));
      var_740 = (uint8_t*)&var_1340;
      var_730 = (uint32_t*)&var_1210;
      _cse1026 = (__entry_r125 - 0x1250);
      var_738 = (uint8_t*)_cse1026;
      t822 = var_1a98;
      var_1210 = ((uint32_t)(t822));
      var_728 = (uint64_t*)&var_1208;
      var_1208 = t822;
      var_720 = (uint32_t*)&var_1200;
      var_718 = (uint32_t*)(__entry_r125 - 0x11fc);
      var_710 = (uint32_t*)&var_11f8;
      var_708 = (uint32_t*)(__entry_r125 - 0x11f4);
      var_11f8 = 0xacc7e8e40c1c991c;
      var_1200 = 0xe381cf0c7ba2e3fc;
      var_6f8 = (uint8_t*)(_cse1026 | 0x1);
      var_700 = (uint8_t*)(_cse1026 | 0x2);
      var_6f0 = (uint8_t*)(_cse1026 | 0x3);
      var_6e8 = (uint8_t*)(_cse1026 | 0x6);
      var_6e0 = (uint8_t*)(_cse1026 | 0x5);
      var_6d8 = (uint8_t*)(_cse1026 | 0x4);
      var_6d0 = (uint8_t*)(_cse1026 | 0x7);
      var_6c8 = (uint8_t*)(__entry_r125 - 0x1246);
      var_6c0 = (uint8_t*)(__entry_r125 - 0x1247);
      var_6b8 = (uint8_t*)(__entry_r125 - 0x1248);
      var_6b0 = (uint8_t*)(__entry_r125 - 0x1245);
      var_6a8 = (uint8_t*)(__entry_r125 - 0x1242);
      var_6a0 = (uint8_t*)(__entry_r125 - 0x1243);
      var_698 = (uint8_t*)(__entry_r125 - 0x1244);
      var_690 = (uint8_t*)(__entry_r125 - 0x1241);
      var_688 = (uint8_t*)(__entry_r125 - 0x123e);
      var_680 = (uint8_t*)(__entry_r125 - 0x123f);
      var_678 = (uint8_t*)(__entry_r125 - 0x1240);
      var_670 = (uint8_t*)(__entry_r125 - 0x123d);
      var_660 = (uint8_t*)(__entry_r125 - 0x123b);
      var_668 = (uint8_t*)(__entry_r125 - 0x123a);
      var_658 = (uint8_t*)(__entry_r125 - 0x123c);
      var_650 = (uint8_t*)(__entry_r125 - 0x1239);
      var_648 = (uint8_t*)(__entry_r125 - 0x1236);
      var_640 = (uint8_t*)(__entry_r125 - 0x1237);
      var_638 = (uint8_t*)(__entry_r125 - 0x1238);
      var_630 = (uint8_t*)(__entry_r125 - 0x1235);
      var_628 = (uint8_t*)(__entry_r125 - 0x1232);
      var_620 = (uint8_t*)(__entry_r125 - 0x1233);
      var_618 = (uint8_t*)(__entry_r125 - 0x1234);
      var_610 = (uint8_t*)(__entry_r125 - 0x1231);
      var_608 = (uint8_t*)(__entry_r125 - 0x122e);
      var_600 = (uint8_t*)(__entry_r125 - 0x122f);
      var_5f8 = (uint8_t*)(__entry_r125 - 0x1230);
      var_5f0 = (uint8_t*)(__entry_r125 - 0x122d);
      var_5e8 = (uint8_t*)(__entry_r125 - 0x122a);
      var_5e0 = (uint8_t*)(__entry_r125 - 0x122b);
      var_5d8 = (uint8_t*)(__entry_r125 - 0x122c);
      var_5c8 = (uint8_t*)(__entry_r125 - 0x1226);
      var_5d0 = (uint8_t*)(__entry_r125 - 0x1229);
      var_5c0 = (uint8_t*)(__entry_r125 - 0x1227);
      var_5b8 = (uint8_t*)(__entry_r125 - 0x1228);
      var_5b0 = (uint8_t*)(__entry_r125 - 0x1225);
      var_5a8 = (uint8_t*)(__entry_r125 - 0x1222);
      var_5a0 = (uint8_t*)(__entry_r125 - 0x1223);
      var_598 = (uint8_t*)(__entry_r125 - 0x1224);
      var_590 = (uint8_t*)(__entry_r125 - 0x1221);
      var_588 = (uint8_t*)(__entry_r125 - 0x121e);
      var_580 = (uint8_t*)(__entry_r125 - 0x121f);
      var_578 = (uint8_t*)(__entry_r125 - 0x1220);
      var_570 = (uint8_t*)(__entry_r125 - 0x121d);
      var_568 = (uint8_t*)(__entry_r125 - 0x121a);
      var_560 = (uint8_t*)(__entry_r125 - 0x121b);
      var_558 = (uint8_t*)(__entry_r125 - 0x121c);
      var_550 = (uint8_t*)(__entry_r125 - 0x1219);
      var_548 = (uint8_t*)(__entry_r125 - 0x1216);
      var_540 = (uint8_t*)(__entry_r125 - 0x1217);
      var_538 = (uint8_t*)(__entry_r125 - 0x1218);
      var_530 = (uint8_t*)(__entry_r125 - 0x1215);
      var_528 = (uint8_t*)(__entry_r125 - 0x1212);
      var_520 = (uint8_t*)(__entry_r125 - 0x1213);
      var_518 = (uint8_t*)(__entry_r125 - 0x1214);
      var_510 = (uint8_t*)(__entry_r125 - 0x1211);
      var_1364 = 0xa259f5ae;
      var_1a60 = var_1210;
      var_1a68 = t822;
      t0 = t822;
      t1 = (((int64_t)(int32_t)(t815)) + ((int64_t)(int32_t)(t814)));
      t2 = (((int64_t)(int32_t)(t817)) + ((int64_t)(int32_t)(t816)));
      t3 = (((int64_t)(int32_t)(t819)) + ((int64_t)(int32_t)(t818)));
      t4 = (((int64_t)(int32_t)(t821)) + ((int64_t)(int32_t)(t820)));
      goto L_0x10004d40;
    }
    // b219 @0x10008c40
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xdff5c815) == 0x1)) {
      // b221 @0x10008c5c
      t278 = var_13f8;
      t279 = var_1aa8;
      _cse1027 = (__entry_r125 - 0x1180);
      t280 = (*(uint32_t*)((_cse1027 + (((t278 + var_1b18) & 0xffffffff) << 0x2))));
      t281 = (*(uint32_t*)((_cse1027 + (((t278 + 0xfffffff9) & 0xffffffff) << 0x2))));
      t282 = (*(uint32_t*)((_cse1027 + (((t278 + var_1ad8) & 0xffffffff) << 0x2))));
      _cse1028 = (((int64_t)(int32_t)((*(uint32_t*)((_cse1027 + (((t278 + 0xfffffffe) & 0xffffffff) << 0x2)))))) & 0xffffffff);
      _cse1029 = (((int64_t)(int32_t)(t280)) & 0xffffffff);
      (*(uint32_t*)((_cse1027 + (t278 << 0x2)))) = ((uint32_t)((((((int64_t)(int32_t)(t282)) + ((int64_t)(int32_t)(t281))) + ((((_cse1028 >> 0x5) | (_cse1028 << 0x1b)) ^ (_cse1028 >> 0x2)) ^ ((_cse1028 >> 0x9) | (_cse1028 << 0x17)))) + ((((_cse1029 >> 0xb) | (_cse1029 << 0x15)) ^ (_cse1029 >> 0x5)) ^ ((_cse1029 >> 0x3) | (_cse1029 << 0x1d))))));
      var_13f8 = (t278 + 0x1);
      if (((var_13f8 == t279) == 0x1)) {
        var_1364 = 0x39a41b0f;
      } else {
        var_1364 = 0xffffffffdff5c815;
      }
      if ((((t278 + 0x1) == t279) == 0x1)) {
        t0 = 0x39a41b0f;
      } else {
        t0 = 0xffffffffdff5c815;
      }
      _cse1030 = ((int64_t)(int32_t)(t282));
      t1 = (_cse1030 + ((int64_t)(int32_t)(t281)));
      _cse1031 = (((int64_t)(int32_t)(t280)) & 0xffffffff);
      t2 = ((((_cse1031 >> 0xb) | (_cse1031 << 0x15)) ^ (_cse1031 >> 0x5)) ^ ((_cse1031 >> 0x3) | (_cse1031 << 0x1d)));
      t3 = _cse1030;
      t4 = (((t278 + 0xfffffff9) & 0xffffffff) << 0x2);
    }
    goto L_0x10004d40;
L_0x10006508:
    // b37 @0x10006508
    if ((((int64_t)(0xe364fcc8) < (int64_t)((((int64_t)(int32_t)(t8)) & 0xffffffff))) == 0x1)) {
      // b187 @0x10007d30
      if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xe364fcc9) != 0x1)) {
        goto L_0x10007d4c;
      }
      // b309 @0x10011200
      t647 = var_c2c;
      t648 = var_41c;
      _cse1032 = (((int64_t)(int32_t)(t648)) + ((int64_t)(int32_t)(t647)));
      (*var_f50) = ((uint32_t)(_cse1032));
      t649 = var_c28;
      t650 = var_420;
      _cse1033 = (((int64_t)(int32_t)(t650)) + ((int64_t)(int32_t)(t649)));
      (*var_f48) = ((uint32_t)(_cse1033));
      t651 = var_c24;
      t652 = var_424;
      _cse1034 = (((int64_t)(int32_t)(t652)) + ((int64_t)(int32_t)(t651)));
      (*var_f40) = ((uint32_t)(_cse1034));
      t653 = var_c20;
      t654 = var_c0c;
      _cse1035 = (((int64_t)(int32_t)(t654)) + ((int64_t)(int32_t)(t653)));
      (*var_f38) = ((uint32_t)(_cse1035));
      t655 = var_c1c;
      t656 = var_428;
      _cse1036 = (((int64_t)(int32_t)(t656)) + ((int64_t)(int32_t)(t655)));
      (*var_f30) = ((uint32_t)(_cse1036));
      t657 = var_c18;
      t658 = var_418;
      _cse1037 = (((int64_t)(int32_t)(t658)) + ((int64_t)(int32_t)(t657)));
      (*var_f28) = ((uint32_t)(_cse1037));
      t659 = var_c14;
      t660 = var_414;
      _cse1038 = (((int64_t)(int32_t)(t660)) + ((int64_t)(int32_t)(t659)));
      (*var_f20) = ((uint32_t)(_cse1038));
      t661 = var_c10;
      t662 = var_c08;
      _cse1039 = (((int64_t)(int32_t)(t662)) + ((int64_t)(int32_t)(t661)));
      (*var_f18) = ((uint32_t)(_cse1039));
      var_c02 = ((uint8_t)(_cse1040));
      _cse1040 = (_cse1032 >> 0x18);
      (*var_fd8) = var_c02;
      var_c01 = ((uint8_t)(_cse1041));
      _cse1041 = (_cse1033 >> 0x18);
      var_c00 = (uint8_t*)&var_132c;
      var_132c = var_c01;
      var_bf1 = ((uint8_t)(_cse1042));
      _cse1042 = (_cse1034 >> 0x18);
      var_bf0 = (uint8_t*)&var_1328;
      var_1328 = var_bf1;
      var_be1 = ((uint8_t)(_cse1043));
      _cse1043 = (_cse1035 >> 0x18);
      var_be0 = (uint8_t*)&var_1324;
      var_1324 = var_be1;
      var_bd1 = ((uint8_t)(_cse1044));
      _cse1044 = (_cse1036 >> 0x18);
      var_bd0 = (uint8_t*)&var_1320;
      var_1320 = var_bd1;
      var_bc1 = ((uint8_t)(_cse1045));
      _cse1045 = (_cse1037 >> 0x18);
      var_bc0 = (uint8_t*)&var_131c;
      var_131c = var_bc1;
      var_bb1 = ((uint8_t)(_cse1046));
      _cse1046 = (_cse1038 >> 0x18);
      var_bb0 = (uint8_t*)&var_1318;
      var_1318 = var_bb1;
      var_ba1 = ((uint8_t)(_cse1047));
      _cse1047 = (_cse1039 >> 0x18);
      var_ba0 = (uint8_t*)&var_1314;
      var_1314 = var_ba1;
      var_b91 = ((uint8_t)(_cse1048));
      _cse1048 = (_cse1032 >> 0x10);
      _cse1050 = (__entry_r125 - 0x1330);
      _cse1049 = (_cse1050 | 0x1);
      var_b90 = (uint8_t*)_cse1049;
      (*(uint8_t*)(_cse1049)) = var_b91;
      var_b81 = ((uint8_t)(_cse1051));
      _cse1051 = (_cse1033 >> 0x10);
      var_b80 = (uint8_t*)&var_132b;
      var_132b = var_b81;
      var_b71 = ((uint8_t)(_cse1052));
      _cse1052 = (_cse1034 >> 0x10);
      var_b70 = (uint8_t*)&var_1327;
      var_1327 = var_b71;
      var_b61 = ((uint8_t)(_cse1053));
      _cse1053 = (_cse1035 >> 0x10);
      var_b60 = (uint8_t*)&var_1323;
      var_1323 = var_b61;
      var_b51 = ((uint8_t)(_cse1054));
      _cse1054 = (_cse1036 >> 0x10);
      var_b50 = (uint8_t*)&var_131f;
      var_131f = var_b51;
      var_b41 = ((uint8_t)(_cse1055));
      _cse1055 = (_cse1037 >> 0x10);
      var_b40 = (uint8_t*)&var_131b;
      var_131b = var_b41;
      var_b31 = ((uint8_t)(_cse1056));
      _cse1056 = (_cse1038 >> 0x10);
      var_b30 = (uint8_t*)&var_1317;
      var_1317 = var_b31;
      var_b21 = ((uint8_t)(_cse1057));
      _cse1057 = (_cse1039 >> 0x10);
      var_b20 = (uint8_t*)&var_1313;
      var_1313 = var_b21;
      var_b11 = ((uint8_t)(_cse1058));
      _cse1058 = (_cse1032 >> 0x8);
      _cse1059 = (_cse1050 | 0x2);
      var_b10 = (uint8_t*)_cse1059;
      (*(uint8_t*)(_cse1059)) = var_b11;
      var_b01 = ((uint8_t)(_cse1060));
      _cse1060 = (_cse1033 >> 0x8);
      var_b00 = (uint8_t*)&var_132a;
      var_132a = var_b01;
      var_af1 = ((uint8_t)(_cse1061));
      _cse1061 = (_cse1034 >> 0x8);
      var_af0 = (uint8_t*)&var_1326;
      var_1326 = var_af1;
      var_ae1 = ((uint8_t)(_cse1062));
      _cse1062 = (_cse1035 >> 0x8);
      var_ae0 = (uint8_t*)&var_1322;
      var_1322 = var_ae1;
      var_ad1 = ((uint8_t)(_cse1063));
      _cse1063 = (_cse1036 >> 0x8);
      var_ad0 = (uint8_t*)&var_131e;
      var_131e = var_ad1;
      var_ac1 = ((uint8_t)(_cse1064));
      _cse1064 = (_cse1037 >> 0x8);
      var_ac0 = (uint8_t*)&var_131a;
      var_131a = var_ac1;
      var_ab1 = ((uint8_t)(_cse1065));
      _cse1065 = (_cse1038 >> 0x8);
      var_ab0 = (uint8_t*)&var_1316;
      var_1316 = var_ab1;
      var_aa1 = ((uint8_t)(_cse1066));
      _cse1066 = (_cse1039 >> 0x8);
      var_aa0 = (uint8_t*)&var_1312;
      var_1312 = var_aa1;
      var_a91 = ((uint8_t)(_cse1032));
      _cse1067 = (_cse1050 | 0x3);
      var_a90 = (uint8_t*)_cse1067;
      (*(uint8_t*)(_cse1067)) = var_a91;
      var_a81 = ((uint8_t)(_cse1033));
      var_a80 = (uint8_t*)&var_1329;
      var_1329 = var_a81;
      var_a71 = ((uint8_t)(_cse1034));
      var_a70 = (uint8_t*)&var_1325;
      var_1325 = var_a71;
      var_a61 = ((uint8_t)(_cse1035));
      var_a60 = (uint8_t*)&var_1321;
      var_1321 = var_a61;
      var_a51 = ((uint8_t)(_cse1036));
      var_a50 = (uint8_t*)&var_131d;
      var_131d = var_a51;
      var_a41 = ((uint8_t)(_cse1037));
      var_a40 = (uint8_t*)&var_1319;
      var_1319 = var_a41;
      var_a31 = ((uint8_t)(_cse1038));
      var_a30 = (uint8_t*)&var_1315;
      var_1315 = var_a31;
      var_a21 = ((uint8_t)(_cse1039));
      var_a20 = (uint8_t*)&var_1311;
      var_1311 = var_a21;
      if (((var_fe0 < var_ff8) == 0x1)) {
        var_1364 = 0x28f426de;
      } else {
        var_1364 = 0xffffffffc5891d49;
      }
      t0 = (((int64_t)(int32_t)(t654)) + ((int64_t)(int32_t)(t653)));
      t1 = (((int64_t)(int32_t)(t656)) + ((int64_t)(int32_t)(t655)));
      t2 = (((int64_t)(int32_t)(t658)) + ((int64_t)(int32_t)(t657)));
      t3 = (((int64_t)(int32_t)(t660)) + ((int64_t)(int32_t)(t659)));
      t4 = (((int64_t)(int32_t)(t662)) + ((int64_t)(int32_t)(t661)));
      goto L_0x10004d40;
    }
    // b39 @0x10006524
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xe0bbcd6f) == 0x1)) {
      // b279 @0x1000a978
      (*var_ef0) = ((uint32_t)((((((((int64_t)(int8_t)((*var_f08))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_f68))) << 0x18)) | ((((int64_t)(int8_t)((*var_f00))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_ef8))) & 0xff))));
      (*var_ec8) = ((uint32_t)((((((((int64_t)(int8_t)((*var_ee0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_ee8))) << 0x18)) | ((((int64_t)(int8_t)((*var_ed8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_ed0))) & 0xff))));
      (*var_ea0) = ((uint32_t)((((((((int64_t)(int8_t)((*var_eb8))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_ec0))) << 0x18)) | ((((int64_t)(int8_t)((*var_eb0))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_ea8))) & 0xff))));
      (*var_e78) = ((uint32_t)((((((((int64_t)(int8_t)((*var_e90))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_e98))) << 0x18)) | ((((int64_t)(int8_t)((*var_e88))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_e80))) & 0xff))));
      (*var_e50) = ((uint32_t)((((((((int64_t)(int8_t)((*var_e68))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_e70))) << 0x18)) | ((((int64_t)(int8_t)((*var_e60))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_e58))) & 0xff))));
      (*var_e28) = ((uint32_t)((((((((int64_t)(int8_t)((*var_e40))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_e48))) << 0x18)) | ((((int64_t)(int8_t)((*var_e38))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_e30))) & 0xff))));
      (*var_e00) = ((uint32_t)((((((((int64_t)(int8_t)((*var_e18))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_e20))) << 0x18)) | ((((int64_t)(int8_t)((*var_e10))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_e08))) & 0xff))));
      (*var_dd8) = ((uint32_t)((((((((int64_t)(int8_t)((*var_df0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_df8))) << 0x18)) | ((((int64_t)(int8_t)((*var_de8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_de0))) & 0xff))));
      (*var_db0) = ((uint32_t)((((((((int64_t)(int8_t)((*var_dc8))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_dd0))) << 0x18)) | ((((int64_t)(int8_t)((*var_dc0))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_db8))) & 0xff))));
      (*var_d88) = ((uint32_t)((((((((int64_t)(int8_t)((*var_da0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_da8))) << 0x18)) | ((((int64_t)(int8_t)((*var_d98))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_d90))) & 0xff))));
      (*var_d60) = ((uint32_t)((((((((int64_t)(int8_t)((*var_d78))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_d80))) << 0x18)) | ((((int64_t)(int8_t)((*var_d70))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_d68))) & 0xff))));
      (*var_d38) = ((uint32_t)((((((((int64_t)(int8_t)((*var_d50))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_d58))) << 0x18)) | ((((int64_t)(int8_t)((*var_d48))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_d40))) & 0xff))));
      (*var_d10) = ((uint32_t)((((((((int64_t)(int8_t)((*var_d28))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_d30))) << 0x18)) | ((((int64_t)(int8_t)((*var_d20))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_d18))) & 0xff))));
      (*var_ce8) = ((uint32_t)((((((((int64_t)(int8_t)((*var_d00))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_d08))) << 0x18)) | ((((int64_t)(int8_t)((*var_cf8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_cf0))) & 0xff))));
      (*var_cc0) = ((uint32_t)((((((((int64_t)(int8_t)((*var_cd8))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_ce0))) << 0x18)) | ((((int64_t)(int8_t)((*var_cd0))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_cc8))) & 0xff))));
      (*var_c98) = ((uint32_t)((((((((int64_t)(int8_t)((*var_cb0))) & 0xff) << 0x10) | (((int64_t)(int8_t)((*var_cb8))) << 0x18)) | ((((int64_t)(int8_t)((*var_ca8))) & 0xff) << 0x8)) | (((int64_t)(int8_t)((*var_ca0))) & 0xff))));
      var_16c0 = var_1b10;
      var_1364 = 0xe179c244;
      goto L_0x10004d40;
    }
    // b41 @0x10006540
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xe0dad150) == 0x1)) {
      // b280 @0x1000b0fc
      var_1d38 = t5;
      var_1d40 = t6;
      var_1d48 = var_16a8;
      var_1d50 = var_16a0;
      var_1d58 = var_1698;
      t315 = var_1690;
      t316 = var_1688;
      t317 = var_1680;
      t318 = var_1678;
      t319 = var_1670;
      t320 = var_1668;
      t321 = var_1660;
      t322 = var_1658;
      t323 = var_1650;
      t324 = var_1648;
      t325 = var_1640;
      t326 = var_1638;
      t327 = var_1630;
      t328 = var_1628;
      t329 = var_1620;
      t330 = var_1618;
      t331 = var_1610;
      t332 = var_1608;
      t333 = var_1600;
      t334 = var_15f8;
      t335 = var_15f0;
      t336 = var_15e8;
      t337 = var_15e0;
      t338 = var_15d8;
      t339 = var_15d0;
      t340 = var_15c8;
      t341 = var_15c0;
      t342 = var_15b8;
      t343 = var_15b0;
      t344 = var_15a8;
      t345 = var_15a0;
      t346 = var_1598;
      t347 = var_1590;
      t348 = var_1588;
      t349 = var_1580;
      t350 = var_1578;
      t351 = var_1570;
      t352 = var_1568;
      t353 = var_1560;
      t354 = var_1550;
      t355 = var_1558;
      t356 = var_1548;
      t357 = var_1540;
      t358 = var_1538;
      t359 = var_1530;
      t360 = var_1528;
      t361 = var_1520;
      t362 = var_1518;
      t363 = var_1510;
      t364 = var_1508;
      t365 = var_1500;
      t366 = var_14f8;
      t367 = var_14f0;
      t368 = var_14e8;
      t369 = var_14e0;
      t370 = var_14d8;
      t371 = var_14d0;
      t372 = var_14c8;
      t373 = var_14b8;
      t374 = var_14c0;
      t375 = var_14b0;
      t376 = var_14a8;
      t377 = var_14a0;
      t378 = var_1498;
      t379 = var_1490;
      t380 = var_1488;
      t381 = var_1480;
      t382 = var_1478;
      t383 = var_1470;
      t384 = var_1468;
      t385 = var_1460;
      t386 = var_1458;
      t387 = var_1450;
      t388 = var_1448;
      t389 = var_1440;
      t390 = var_1438;
      t391 = var_1430;
      /* 1 argument slot(s) hold values this function never wrote: either the callee reads a stale register or a definition was lost here */
      t392 = vm_memset((((__entry_r125 - 0x1080) + var_16b8) + 0x1), 0x0, (((0x36 - ((int64_t)(int32_t)(var_16ac))) & 0xffffffff) + 0x1), 0x10016a2c, t1, t2, t3, t4);
      var_1364 = 0x19b80fb0;
      var_16f8 = t391;
      var_1700 = t390;
      var_1708 = t389;
      var_1710 = t388;
      var_1718 = t387;
      var_1720 = t386;
      var_1728 = t385;
      var_1730 = t384;
      var_1738 = t383;
      var_1740 = t382;
      var_1748 = t381;
      var_1750 = t380;
      var_1758 = t379;
      var_1760 = t378;
      var_1768 = t377;
      var_1770 = t376;
      var_1778 = t375;
      var_1788 = t374;
      var_1780 = t373;
      var_1790 = t372;
      var_1798 = t371;
      var_17a0 = t370;
      var_17a8 = t369;
      var_17b0 = t368;
      var_17b8 = t367;
      var_17c0 = t366;
      var_17c8 = t365;
      var_17d0 = t364;
      var_17d8 = t363;
      var_17e0 = t362;
      var_17e8 = t361;
      var_17f0 = t360;
      var_17f8 = t359;
      var_1800 = t358;
      var_1808 = t357;
      var_1810 = t356;
      var_1820 = t355;
      var_1818 = t354;
      var_1828 = t353;
      var_1830 = t352;
      var_1838 = t351;
      var_1840 = t350;
      var_1848 = t349;
      var_1850 = t348;
      var_1858 = t347;
      var_1860 = t346;
      var_1868 = t345;
      var_1870 = t344;
      var_1878 = t343;
      var_1880 = t342;
      var_1888 = t341;
      var_1890 = t340;
      var_1898 = t339;
      var_18a0 = t338;
      var_18a8 = t337;
      var_18b0 = t336;
      var_18b8 = t335;
      var_18c0 = t334;
      var_18c8 = t333;
      var_18d0 = t332;
      var_18d8 = t331;
      var_18e0 = t330;
      var_18e8 = t329;
      var_18f0 = t328;
      var_18f8 = t327;
      var_1900 = t326;
      var_1908 = t325;
      var_1910 = t324;
      var_1918 = t323;
      var_1920 = t322;
      var_1928 = t321;
      var_1930 = t320;
      var_1938 = t319;
      var_1940 = t318;
      var_1948 = t317;
      var_1950 = t316;
      var_1958 = t315;
      var_1960 = var_1d58;
      var_1968 = var_1d50;
      var_1970 = var_1d48;
      t393 = var_1d40;
      t394 = var_1d38;
      t0 = t1 = t2 = t3 = t4 = 0x0; /* t0, t1, t2, t3, t4: never set up on this edge */
      t5 = t394;
      t6 = t393;
      goto L_0x10004d40;
    }
    // b43 @0x1000655c
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xe179c244) == 0x1)) {
      // b45 @0x10006578
      t160 = var_16c0;
      t161 = var_1aa8;
      _cse1068 = (__entry_r125 - 0x1180);
      t162 = (*(uint32_t*)((_cse1068 + (((t160 + var_1b18) & 0xffffffff) << 0x2))));
      t163 = (*(uint32_t*)((_cse1068 + (((t160 + 0xfffffff9) & 0xffffffff) << 0x2))));
      t164 = (*(uint32_t*)((_cse1068 + (((t160 + var_1ad8) & 0xffffffff) << 0x2))));
      _cse1069 = (((int64_t)(int32_t)((*(uint32_t*)((_cse1068 + (((t160 + 0xfffffffe) & 0xffffffff) << 0x2)))))) & 0xffffffff);
      _cse1070 = (((int64_t)(int32_t)(t162)) & 0xffffffff);
      (*(uint32_t*)((_cse1068 + (t160 << 0x2)))) = ((uint32_t)((((((int64_t)(int32_t)(t164)) + ((int64_t)(int32_t)(t163))) + ((((_cse1069 >> 0x5) | (_cse1069 << 0x1b)) ^ (_cse1069 >> 0x2)) ^ ((_cse1069 >> 0x9) | (_cse1069 << 0x17)))) + ((((_cse1070 >> 0xb) | (_cse1070 << 0x15)) ^ (_cse1070 >> 0x5)) ^ ((_cse1070 >> 0x3) | (_cse1070 << 0x1d))))));
      var_16c0 = (t160 + 0x1);
      if (((var_16c0 == t161) == 0x1)) {
        var_1364 = 0x2eb81b39;
      } else {
        var_1364 = 0xffffffffe179c244;
      }
      if ((((t160 + 0x1) == t161) == 0x1)) {
        t0 = 0x2eb81b39;
      } else {
        t0 = 0xffffffffe179c244;
      }
      _cse1071 = ((int64_t)(int32_t)(t164));
      t1 = (_cse1071 + ((int64_t)(int32_t)(t163)));
      _cse1072 = (((int64_t)(int32_t)(t162)) & 0xffffffff);
      t2 = ((((_cse1072 >> 0xb) | (_cse1072 << 0x15)) ^ (_cse1072 >> 0x5)) ^ ((_cse1072 >> 0x3) | (_cse1072 << 0x1d)));
      t3 = _cse1071;
      t4 = (((t160 + 0xfffffff9) & 0xffffffff) << 0x2);
    }
    goto L_0x10004d40;
L_0x10007d4c:
    // b189 @0x10007d4c
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xe3c9c744) == 0x1)) {
      // b310 @0x10011aec
      t663 = var_12d8;
      t664 = var_fe0;
      (*(uint64_t*)((t664 + 0x30))) = var_12e0;
      (*(uint64_t*)((t664 + 0x38))) = t663;
      t665 = var_12f0;
      t666 = var_fe0;
      (*(uint64_t*)((t666 + 0x28))) = var_12e8;
      (*(uint64_t*)((t666 + 0x20))) = t665;
      t667 = var_1300;
      t668 = var_fe0;
      (*(uint64_t*)((t668 + 0x18))) = var_12f8;
      (*(uint64_t*)((t668 + 0x10))) = t667;
      t669 = var_1310;
      t670 = var_fe0;
      (*(uint64_t*)((t670 + 0x8))) = var_1308;
      (*(uint64_t*)(t670)) = t669;
      var_1364 = 0x3ba517f7;
      goto L_0x10004d40;
    }
    // b191 @0x10007d68
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xe8038afc) == 0x1)) {
      // b193 @0x10007d84
      t195 = var_1ad0;
      var_fa0 = ((uint64_t)(var_fd0) + var_fa8);
      var_f98 = (var_1b30 + var_fa8);
      var_13c0 = t195;
      if (((t195 < 0x20) == 0x1)) {
        var_1364 = 0xffffffff839fc771;
      } else {
        var_1364 = 0x5c2011df;
      }
      var_13b0 = var_fa0;
      var_13b8 = var_f98;
      t0 = t195;
    }
    goto L_0x10004d40;
L_0x10007574:
    // b123 @0x10007574
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xf23572f0) == 0x1)) {
      // b294 @0x1000cc48
      var_1351 = var_4e9;
      (*(uint8_t*)(&var_1356)) = (*(uint8_t*)(&var_1336));
      var_1357 = var_1337;
      (*(uint8_t*)(&var_1358)) = (*(uint8_t*)(&var_1338));
      var_1360 = var_1340;
      var_1364 = 0x5e619a9b;
      var_1352 = var_4ec;
      var_1353 = var_4ee;
      (*(uint8_t*)(&var_1354)) = var_4f1;
      var_1355 = var_4ea;
      var_1a78 = (uint8_t*)&var_1351;
      goto L_0x10004d40;
    }
    // b125 @0x10007590
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xf40434cd) == 0x1)) {
      // b127 @0x100075ac
      if (((var_500 < 0x38) == 0x1)) {
        var_1364 = 0x68901447;
      } else {
        var_1364 = 0x263afc19;
      }
    }
    goto L_0x10004d40;
L_0x1000979c:
    // b245 @0x1000979c
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xfad73dca) == 0x1)) {
      // b326 @0x10015624
      t884 = var_1b20;
      var_1378 = var_1b30;
      var_1380 = var_fd0;
      var_1388 = var_1ad0;
      if (((var_1ad0 < 0x20) == 0x1)) {
        var_1364 = t884;
      } else {
        var_1364 = 0xfffffffff4ab7145;
      }
      t0 = t884;
      goto L_0x10004d40;
    }
    // b247 @0x100097b8
    if ((((((int64_t)(int32_t)(t8)) & 0xffffffff) == 0xfb03d450) == 0x1)) {
      // b249 @0x100097d4
      var_1378 = var_fc0;
      if (((var_fc8 == var_1ad0) == 0x1)) {
        var_1364 = 0xffffffff89fd6da1;
      } else {
        var_1364 = var_1b20;
      }
      var_1380 = var_fb8;
      var_1388 = var_fb0;
    }
    goto L_0x10004d40;
  }
  goto L_0x10004d78;
}
