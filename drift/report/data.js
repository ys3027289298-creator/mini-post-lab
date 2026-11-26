window.DRIFT_DATA = {
  "fps": 60,
  "q": 0.00390625,
  "budget": 0.001953125,
  "budget_quant": 0.5,
  "n10": 36000,
  "policy_r": 600,
  "policy_seconds": 10.0,
  "estimates": {
    "q_1_256": {
      "quantum": 0.00390625,
      "budget": 0.001953125,
      "n10": 36000,
      "delta_per_frame": 5.4253472222222225e-08,
      "sigma_per_frame_rms": 1.0293872591693944e-05,
      "delta_per_second": 3.2552083333333335e-06,
      "sigma_per_sqrt_second": 7.973599423122325e-05,
      "reset_linear_s": 600.0,
      "reset_sqrt_s": 599.9999999999999,
      "reset_linear_80pct_s": 480.0,
      "reset_sqrt_80pct_s": 384.00000000000006
    },
    "q_1_255": {
      "quantum": 0.00392156862745098,
      "budget": 0.00196078431372549,
      "n10": 36000,
      "delta_per_frame": 5.4466230936819174e-08,
      "sigma_per_frame_rms": 1.0334240719504508e-05,
      "delta_per_second": 3.26797385620915e-06,
      "sigma_per_sqrt_second": 8.004868440467902e-05,
      "reset_linear_s": 600.0,
      "reset_sqrt_s": 599.9999999999999,
      "reset_linear_80pct_s": 480.0,
      "reset_sqrt_80pct_s": 384.00000000000006
    }
  },
  "fits": {
    "linear": {
      "alpha": 0.999981620170544,
      "sse_linear": 2.9989645178666913e-09,
      "sse_sqrt": 2.1061749387566095,
      "decision": "linear"
    },
    "sqrt": {
      "alpha": 0.5014464439537956,
      "sse_linear": 2.0943846956672894,
      "sse_sqrt": 0.00024165594778817424,
      "decision": "sqrt"
    }
  },
  "runs": [
    {
      "model": "linear",
      "tier": "1min",
      "duration_s": 60,
      "duration_min": 1.0,
      "frames": 3600,
      "lanes": 4096,
      "signed_mean_norm": 0.000195310669369,
      "rms_norm": 0.000195310669369,
      "max_abs_norm": 0.000195310669369,
      "offset_quant": 0.049999531358464,
      "signed_mean_quant": 0.049999531358464,
      "max_abs_quant": 0.049999531358464,
      "policy_r_frames": 600,
      "boundary_rms_norm": 3.25519577018e-05,
      "boundary_rms_quant": 0.0083333011716608,
      "policy_max_abs_norm": 3.25520833333e-05,
      "policy_max_abs_quant": 0.0083333333333248,
      "bare_pass": true,
      "policy_pass": true
    },
    {
      "model": "linear",
      "tier": "10min",
      "duration_s": 600,
      "duration_min": 10.0,
      "frames": 36000,
      "lanes": 4096,
      "signed_mean_norm": 0.00195299577899,
      "rms_norm": 0.00195299577899,
      "max_abs_norm": 0.00195299577899,
      "offset_quant": 0.49996691942144,
      "signed_mean_quant": 0.49996691942144,
      "max_abs_quant": 0.49996691942144,
      "policy_r_frames": 600,
      "boundary_rms_norm": 3.25519577018e-05,
      "boundary_rms_quant": 0.0083333011716608,
      "policy_max_abs_norm": 3.25520833333e-05,
      "policy_max_abs_quant": 0.0083333333333248,
      "bare_pass": true,
      "policy_pass": true
    },
    {
      "model": "linear",
      "tier": "60min",
      "duration_s": 3600,
      "duration_min": 60.0,
      "frames": 216000,
      "lanes": 4096,
      "signed_mean_norm": 0.0117177730426,
      "rms_norm": 0.0117177730426,
      "max_abs_norm": 0.0117177730426,
      "offset_quant": 2.9997498989056,
      "signed_mean_quant": 2.9997498989056,
      "max_abs_quant": 2.9997498989056,
      "policy_r_frames": 600,
      "boundary_rms_norm": 3.25519577015e-05,
      "boundary_rms_quant": 0.008333301171584,
      "policy_max_abs_norm": 3.25520833333e-05,
      "policy_max_abs_quant": 0.0083333333333248,
      "bare_pass": false,
      "policy_pass": true
    },
    {
      "model": "sqrt",
      "tier": "1min",
      "duration_s": 60,
      "duration_min": 1.0,
      "frames": 3600,
      "lanes": 4096,
      "signed_mean_norm": -2.05088667712e-06,
      "rms_norm": 0.000614699584134,
      "max_abs_norm": 0.0025354581885,
      "offset_quant": 0.157363093538304,
      "signed_mean_quant": -0.00052502698934272,
      "max_abs_quant": 0.649077296256,
      "policy_r_frames": 600,
      "boundary_rms_norm": 0.000251890254473,
      "boundary_rms_quant": 0.064483905145088,
      "policy_max_abs_norm": 0.001024540863,
      "policy_max_abs_quant": 0.262282460928,
      "bare_pass": true,
      "policy_pass": true
    },
    {
      "model": "sqrt",
      "tier": "10min",
      "duration_s": 600,
      "duration_min": 10.0,
      "frames": 36000,
      "lanes": 4096,
      "signed_mean_norm": 2.23678928513e-06,
      "rms_norm": 0.00198481397222,
      "max_abs_norm": 0.00723161688074,
      "offset_quant": 0.50811237688832,
      "signed_mean_quant": 0.00057261805699328,
      "max_abs_quant": 1.85129392146944,
      "policy_r_frames": 600,
      "boundary_rms_norm": 0.000252336573487,
      "boundary_rms_quant": 0.064598162812672,
      "policy_max_abs_norm": 0.00122238404583,
      "policy_max_abs_quant": 0.31293031573248,
      "bare_pass": false,
      "policy_pass": true
    },
    {
      "model": "sqrt",
      "tier": "60min",
      "duration_s": 3600,
      "duration_min": 60.0,
      "frames": 216000,
      "lanes": 4096,
      "signed_mean_norm": -4.64074840841e-05,
      "rms_norm": 0.00478244592335,
      "max_abs_norm": 0.0191765483469,
      "offset_quant": 1.2243061563776,
      "signed_mean_quant": -0.0118803159255296,
      "max_abs_quant": 4.9091963768064,
      "policy_r_frames": 600,
      "boundary_rms_norm": 0.000251968832929,
      "boundary_rms_quant": 0.064504021229824,
      "policy_max_abs_norm": 0.00127520086244,
      "policy_max_abs_quant": 0.32645142078464,
      "bare_pass": false,
      "policy_pass": true
    }
  ],
  "runtime_curves": [
    {
      "minutes": 1,
      "linear_quant": 0.05,
      "sqrt_quant": 0.15811388300841897
    },
    {
      "minutes": 2,
      "linear_quant": 0.1,
      "sqrt_quant": 0.22360679774997896
    },
    {
      "minutes": 3,
      "linear_quant": 0.15,
      "sqrt_quant": 0.27386127875258304
    },
    {
      "minutes": 5,
      "linear_quant": 0.25,
      "sqrt_quant": 0.3535533905932738
    },
    {
      "minutes": 7,
      "linear_quant": 0.35,
      "sqrt_quant": 0.4183300132670378
    },
    {
      "minutes": 10,
      "linear_quant": 0.5,
      "sqrt_quant": 0.5
    },
    {
      "minutes": 15,
      "linear_quant": 0.75,
      "sqrt_quant": 0.6123724356957945
    },
    {
      "minutes": 20,
      "linear_quant": 1.0,
      "sqrt_quant": 0.7071067811865476
    },
    {
      "minutes": 30,
      "linear_quant": 1.5,
      "sqrt_quant": 0.8660254037844386
    },
    {
      "minutes": 45,
      "linear_quant": 2.25,
      "sqrt_quant": 1.0606601717798212
    },
    {
      "minutes": 60,
      "linear_quant": 3.0,
      "sqrt_quant": 1.224744871391589
    }
  ],
  "reset_curves": [
    {
      "r": 1,
      "seconds": 0.0166666667,
      "linear_quant": 1.38888888889e-05,
      "sqrt_quant": 0.00263523138347,
      "continuity": 0.0
    },
    {
      "r": 2,
      "seconds": 0.0333333333,
      "linear_quant": 2.77777777778e-05,
      "sqrt_quant": 0.0037267799625,
      "continuity": 0.292893218813
    },
    {
      "r": 3,
      "seconds": 0.05,
      "linear_quant": 4.16666666667e-05,
      "sqrt_quant": 0.00456435464588,
      "continuity": 0.42264973081
    },
    {
      "r": 4,
      "seconds": 0.0666666667,
      "linear_quant": 5.55555555556e-05,
      "sqrt_quant": 0.00527046276695,
      "continuity": 0.5
    },
    {
      "r": 5,
      "seconds": 0.0833333333,
      "linear_quant": 6.94444444444e-05,
      "sqrt_quant": 0.00589255650989,
      "continuity": 0.5527864045
    },
    {
      "r": 6,
      "seconds": 0.1,
      "linear_quant": 8.33333333333e-05,
      "sqrt_quant": 0.00645497224368,
      "continuity": 0.591751709536
    },
    {
      "r": 7,
      "seconds": 0.116666667,
      "linear_quant": 9.72222222222e-05,
      "sqrt_quant": 0.00697216688778,
      "continuity": 0.622035526991
    },
    {
      "r": 8,
      "seconds": 0.133333333,
      "linear_quant": 0.000111111111111,
      "sqrt_quant": 0.007453559925,
      "continuity": 0.646446609407
    },
    {
      "r": 9,
      "seconds": 0.15,
      "linear_quant": 0.000125,
      "sqrt_quant": 0.00790569415042,
      "continuity": 0.666666666667
    },
    {
      "r": 10,
      "seconds": 0.166666667,
      "linear_quant": 0.000138888888889,
      "sqrt_quant": 0.00833333333333,
      "continuity": 0.683772233983
    },
    {
      "r": 11,
      "seconds": 0.183333333,
      "linear_quant": 0.000152777777778,
      "sqrt_quant": 0.00874007373475,
      "continuity": 0.698488655422
    },
    {
      "r": 12,
      "seconds": 0.2,
      "linear_quant": 0.000166666666667,
      "sqrt_quant": 0.00912870929175,
      "continuity": 0.711324865405
    },
    {
      "r": 13,
      "seconds": 0.216666667,
      "linear_quant": 0.000180555555556,
      "sqrt_quant": 0.00950146187583,
      "continuity": 0.722649901887
    },
    {
      "r": 14,
      "seconds": 0.233333333,
      "linear_quant": 0.000194444444444,
      "sqrt_quant": 0.00986013297183,
      "continuity": 0.732738758088
    },
    {
      "r": 16,
      "seconds": 0.266666667,
      "linear_quant": 0.000222222222222,
      "sqrt_quant": 0.0105409255339,
      "continuity": 0.75
    },
    {
      "r": 18,
      "seconds": 0.3,
      "linear_quant": 0.00025,
      "sqrt_quant": 0.0111803398875,
      "continuity": 0.764297739604
    },
    {
      "r": 19,
      "seconds": 0.316666667,
      "linear_quant": 0.000263888888889,
      "sqrt_quant": 0.0114867072934,
      "continuity": 0.770584266129
    },
    {
      "r": 22,
      "seconds": 0.366666667,
      "linear_quant": 0.000305555555556,
      "sqrt_quant": 0.0123603308118,
      "continuity": 0.786799283644
    },
    {
      "r": 24,
      "seconds": 0.4,
      "linear_quant": 0.000333333333333,
      "sqrt_quant": 0.0129099444874,
      "continuity": 0.795875854768
    },
    {
      "r": 26,
      "seconds": 0.433333333,
      "linear_quant": 0.000361111111111,
      "sqrt_quant": 0.0134370962472,
      "continuity": 0.803883864862
    },
    {
      "r": 29,
      "seconds": 0.483333333,
      "linear_quant": 0.000402777777778,
      "sqrt_quant": 0.0141911553049,
      "continuity": 0.814304661823
    },
    {
      "r": 32,
      "seconds": 0.533333333,
      "linear_quant": 0.000444444444444,
      "sqrt_quant": 0.01490711985,
      "continuity": 0.823223304703
    },
    {
      "r": 36,
      "seconds": 0.6,
      "linear_quant": 0.0005,
      "sqrt_quant": 0.0158113883008,
      "continuity": 0.833333333333
    },
    {
      "r": 40,
      "seconds": 0.666666667,
      "linear_quant": 0.000555555555556,
      "sqrt_quant": 0.0166666666667,
      "continuity": 0.841886116992
    },
    {
      "r": 44,
      "seconds": 0.733333333,
      "linear_quant": 0.000611111111111,
      "sqrt_quant": 0.0174801474695,
      "continuity": 0.849244327711
    },
    {
      "r": 49,
      "seconds": 0.816666667,
      "linear_quant": 0.000680555555556,
      "sqrt_quant": 0.0184466196843,
      "continuity": 0.857142857143
    },
    {
      "r": 54,
      "seconds": 0.9,
      "linear_quant": 0.00075,
      "sqrt_quant": 0.019364916731,
      "continuity": 0.863917236512
    },
    {
      "r": 60,
      "seconds": 1.0,
      "linear_quant": 0.000833333333333,
      "sqrt_quant": 0.0204124145232,
      "continuity": 0.870900555126
    },
    {
      "r": 66,
      "seconds": 1.1,
      "linear_quant": 0.000916666666667,
      "sqrt_quant": 0.0214087209644,
      "continuity": 0.876908509021
    },
    {
      "r": 74,
      "seconds": 1.23333333,
      "linear_quant": 0.00102777777778,
      "sqrt_quant": 0.0226691175146,
      "continuity": 0.883752361256
    },
    {
      "r": 82,
      "seconds": 1.36666667,
      "linear_quant": 0.00113888888889,
      "sqrt_quant": 0.0238630351055,
      "continuity": 0.889568473925
    },
    {
      "r": 90,
      "seconds": 1.5,
      "linear_quant": 0.00125,
      "sqrt_quant": 0.025,
      "continuity": 0.894590744661
    },
    {
      "r": 100,
      "seconds": 1.66666667,
      "linear_quant": 0.00138888888889,
      "sqrt_quant": 0.0263523138347,
      "continuity": 0.9
    },
    {
      "r": 111,
      "seconds": 1.85,
      "linear_quant": 0.00154166666667,
      "sqrt_quant": 0.0277638854149,
      "continuity": 0.905084200425
    },
    {
      "r": 123,
      "seconds": 2.05,
      "linear_quant": 0.00170833333333,
      "sqrt_quant": 0.0292261298613,
      "continuity": 0.909833036533
    },
    {
      "r": 136,
      "seconds": 2.26666667,
      "linear_quant": 0.00188888888889,
      "sqrt_quant": 0.0307318148576,
      "continuity": 0.914250707429
    },
    {
      "r": 151,
      "seconds": 2.51666667,
      "linear_quant": 0.00209722222222,
      "sqrt_quant": 0.0323822653795,
      "continuity": 0.918621154123
    },
    {
      "r": 167,
      "seconds": 2.78333333,
      "linear_quant": 0.00231944444444,
      "sqrt_quant": 0.0340546945695,
      "continuity": 0.922617676747
    },
    {
      "r": 185,
      "seconds": 3.08333333,
      "linear_quant": 0.00256944444444,
      "sqrt_quant": 0.035843021946,
      "continuity": 0.926478537791
    },
    {
      "r": 205,
      "seconds": 3.41666667,
      "linear_quant": 0.00284722222222,
      "sqrt_quant": 0.0377307714089,
      "continuity": 0.930156970423
    },
    {
      "r": 227,
      "seconds": 3.78333333,
      "linear_quant": 0.00315277777778,
      "sqrt_quant": 0.0397037641652,
      "continuity": 0.93362766884
    },
    {
      "r": 251,
      "seconds": 4.18333333,
      "linear_quant": 0.00348611111111,
      "sqrt_quant": 0.0417499168329,
      "continuity": 0.93688055969
    },
    {
      "r": 279,
      "seconds": 4.65,
      "linear_quant": 0.003875,
      "sqrt_quant": 0.0440170421541,
      "continuity": 0.940131565991
    },
    {
      "r": 309,
      "seconds": 5.15,
      "linear_quant": 0.00429166666667,
      "sqrt_quant": 0.0463231403656,
      "continuity": 0.943111987601
    },
    {
      "r": 342,
      "seconds": 5.7,
      "linear_quant": 0.00475,
      "sqrt_quant": 0.048733971724,
      "continuity": 0.945926192956
    },
    {
      "r": 379,
      "seconds": 6.31666667,
      "linear_quant": 0.00526388888889,
      "sqrt_quant": 0.0513024799054,
      "continuity": 0.948633450306
    },
    {
      "r": 420,
      "seconds": 7.0,
      "linear_quant": 0.00583333333333,
      "sqrt_quant": 0.0540061724867,
      "continuity": 0.951204996353
    },
    {
      "r": 465,
      "seconds": 7.75,
      "linear_quant": 0.00645833333333,
      "sqrt_quant": 0.0568257570708,
      "continuity": 0.953626110424
    },
    {
      "r": 515,
      "seconds": 8.58333333,
      "linear_quant": 0.00715277777778,
      "sqrt_quant": 0.05980291706,
      "continuity": 0.955934735076
    },
    {
      "r": 570,
      "seconds": 9.5,
      "linear_quant": 0.00791666666667,
      "sqrt_quant": 0.0629152869606,
      "continuity": 0.958114609171
    },
    {
      "r": 632,
      "seconds": 10.5333333,
      "linear_quant": 0.00877777777778,
      "sqrt_quant": 0.0662486897145,
      "continuity": 0.960222135791
    },
    {
      "r": 700,
      "seconds": 11.6666667,
      "linear_quant": 0.00972222222222,
      "sqrt_quant": 0.0697216688778,
      "continuity": 0.962203552699
    },
    {
      "r": 775,
      "seconds": 12.9166667,
      "linear_quant": 0.0107638888889,
      "sqrt_quant": 0.0733617369236,
      "continuity": 0.964078939595
    },
    {
      "r": 859,
      "seconds": 14.3166667,
      "linear_quant": 0.0119305555556,
      "sqrt_quant": 0.0772352107382,
      "continuity": 0.965880440303
    },
    {
      "r": 951,
      "seconds": 15.85,
      "linear_quant": 0.0132083333333,
      "sqrt_quant": 0.0812660240609,
      "continuity": 0.967572778244
    },
    {
      "r": 1054,
      "seconds": 17.5666667,
      "linear_quant": 0.0146388888889,
      "sqrt_quant": 0.0855537517847,
      "continuity": 0.969197944818
    },
    {
      "r": 1168,
      "seconds": 19.4666667,
      "linear_quant": 0.0162222222222,
      "sqrt_quant": 0.0900617072407,
      "continuity": 0.970739713201
    },
    {
      "r": 1293,
      "seconds": 21.55,
      "linear_quant": 0.0179583333333,
      "sqrt_quant": 0.0947584648813,
      "continuity": 0.972190016092
    },
    {
      "r": 1433,
      "seconds": 23.8833333,
      "linear_quant": 0.0199027777778,
      "sqrt_quant": 0.0997566483443,
      "continuity": 0.973583400934
    },
    {
      "r": 1587,
      "seconds": 26.45,
      "linear_quant": 0.0220416666667,
      "sqrt_quant": 0.104980156855,
      "continuity": 0.974897814383
    },
    {
      "r": 1758,
      "seconds": 29.3,
      "linear_quant": 0.0244166666667,
      "sqrt_quant": 0.11049132696,
      "continuity": 0.976149880212
    },
    {
      "r": 1948,
      "seconds": 32.4666667,
      "linear_quant": 0.0270555555556,
      "sqrt_quant": 0.116308975482,
      "continuity": 0.977342837279
    },
    {
      "r": 2158,
      "seconds": 35.9666667,
      "linear_quant": 0.0299722222222,
      "sqrt_quant": 0.122417772856,
      "continuity": 0.978473457555
    },
    {
      "r": 2390,
      "seconds": 39.8333333,
      "linear_quant": 0.0331944444444,
      "sqrt_quant": 0.128830206948,
      "continuity": 0.979544926257
    },
    {
      "r": 2648,
      "seconds": 44.1333333,
      "linear_quant": 0.0367777777778,
      "sqrt_quant": 0.135605637379,
      "continuity": 0.980566948142
    },
    {
      "r": 2934,
      "seconds": 48.9,
      "linear_quant": 0.04075,
      "sqrt_quant": 0.142741024236,
      "continuity": 0.981538374146
    },
    {
      "r": 3250,
      "seconds": 54.1666667,
      "linear_quant": 0.0451388888889,
      "sqrt_quant": 0.150231303144,
      "continuity": 0.982458839614
    },
    {
      "r": 3600,
      "seconds": 60.0,
      "linear_quant": 0.05,
      "sqrt_quant": 0.158113883008,
      "continuity": 0.983333333333
    },
    {
      "r": 3988,
      "seconds": 66.4666667,
      "linear_quant": 0.0553888888889,
      "sqrt_quant": 0.166416478885,
      "continuity": 0.98416484112
    },
    {
      "r": 4418,
      "seconds": 73.6333333,
      "linear_quant": 0.0613611111111,
      "sqrt_quant": 0.175158658237,
      "continuity": 0.984955174868
    },
    {
      "r": 4894,
      "seconds": 81.5666667,
      "linear_quant": 0.0679722222222,
      "sqrt_quant": 0.184353223761,
      "continuity": 0.985705531318
    },
    {
      "r": 5421,
      "seconds": 90.35,
      "linear_quant": 0.0752916666667,
      "sqrt_quant": 0.194025341987,
      "continuity": 0.986418107261
    },
    {
      "r": 6006,
      "seconds": 100.1,
      "linear_quant": 0.0834166666667,
      "sqrt_quant": 0.204226181802,
      "continuity": 0.987096505648
    },
    {
      "r": 6653,
      "seconds": 110.883333,
      "linear_quant": 0.0924027777778,
      "sqrt_quant": 0.214945083426,
      "continuity": 0.987739978317
    },
    {
      "r": 7370,
      "seconds": 122.833333,
      "linear_quant": 0.102361111111,
      "sqrt_quant": 0.226231199342,
      "continuity": 0.988351600526
    },
    {
      "r": 8165,
      "seconds": 136.083333,
      "linear_quant": 0.113402777778,
      "sqrt_quant": 0.23812053437,
      "continuity": 0.988933203974
    },
    {
      "r": 9045,
      "seconds": 150.75,
      "linear_quant": 0.125625,
      "sqrt_quant": 0.250624220697,
      "continuity": 0.989485328369
    },
    {
      "r": 10019,
      "seconds": 166.983333,
      "linear_quant": 0.139152777778,
      "sqrt_quant": 0.263773366527,
      "continuity": 0.990009486484
    },
    {
      "r": 11099,
      "seconds": 184.983333,
      "linear_quant": 0.154152777778,
      "sqrt_quant": 0.277626347613,
      "continuity": 0.990507992465
    },
    {
      "r": 12296,
      "seconds": 204.933333,
      "linear_quant": 0.170777777778,
      "sqrt_quant": 0.292213772586,
      "continuity": 0.990981837166
    },
    {
      "r": 13621,
      "seconds": 227.016667,
      "linear_quant": 0.189180555556,
      "sqrt_quant": 0.307555324743,
      "continuity": 0.991431683436
    },
    {
      "r": 15089,
      "seconds": 251.483333,
      "linear_quant": 0.209569444444,
      "sqrt_quant": 0.323704683658,
      "continuity": 0.991859149662
    },
    {
      "r": 16715,
      "seconds": 278.583333,
      "linear_quant": 0.232152777778,
      "sqrt_quant": 0.340699851613,
      "continuity": 0.992265240589
    },
    {
      "r": 18517,
      "seconds": 308.616667,
      "linear_quant": 0.257180555556,
      "sqrt_quant": 0.3585948658,
      "continuity": 0.992651229466
    },
    {
      "r": 20512,
      "seconds": 341.866667,
      "linear_quant": 0.284888888889,
      "sqrt_quant": 0.377418129459,
      "continuity": 0.993017740332
    },
    {
      "r": 22723,
      "seconds": 378.716667,
      "linear_quant": 0.315597222222,
      "sqrt_quant": 0.397238733146,
      "continuity": 0.993366126806
    },
    {
      "r": 25172,
      "seconds": 419.533333,
      "linear_quant": 0.349611111111,
      "sqrt_quant": 0.418097543111,
      "continuity": 0.993697089526
    },
    {
      "r": 27885,
      "seconds": 464.75,
      "linear_quant": 0.387291666667,
      "sqrt_quant": 0.440052080251,
      "continuity": 0.994011546583
    },
    {
      "r": 30891,
      "seconds": 514.85,
      "linear_quant": 0.429041666667,
      "sqrt_quant": 0.463163937859,
      "continuity": 0.994310370113
    },
    {
      "r": 34220,
      "seconds": 570.333333,
      "linear_quant": 0.475277777778,
      "sqrt_quant": 0.487482193407,
      "continuity": 0.994594199708
    },
    {
      "r": 37909,
      "seconds": 631.816667,
      "linear_quant": 0.526513888889,
      "sqrt_quant": 0.513085708673,
      "continuity": 0.994863954815
    },
    {
      "r": 41995,
      "seconds": 699.916667,
      "linear_quant": 0.583263888889,
      "sqrt_quant": 0.540029577379,
      "continuity": 0.995120209163
    },
    {
      "r": 46521,
      "seconds": 775.35,
      "linear_quant": 0.646125,
      "sqrt_quant": 0.568385872449,
      "continuity": 0.99536365784
    },
    {
      "r": 51535,
      "seconds": 858.916667,
      "linear_quant": 0.715763888889,
      "sqrt_quant": 0.598232349881,
      "continuity": 0.995594970108
    },
    {
      "r": 57089,
      "seconds": 951.483333,
      "linear_quant": 0.792902777778,
      "sqrt_quant": 0.629643858772,
      "continuity": 0.995814727093
    },
    {
      "r": 63242,
      "seconds": 1054.03333,
      "linear_quant": 0.878361111111,
      "sqrt_quant": 0.662706990725,
      "continuity": 0.996023534654
    },
    {
      "r": 70059,
      "seconds": 1167.65,
      "linear_quant": 0.973041666667,
      "sqrt_quant": 0.697510453924,
      "continuity": 0.996221947114
    },
    {
      "r": 77610,
      "seconds": 1293.5,
      "linear_quant": 1.07791666667,
      "sqrt_quant": 0.734137816308,
      "continuity": 0.996410440486
    },
    {
      "r": 85975,
      "seconds": 1432.91667,
      "linear_quant": 1.19409722222,
      "sqrt_quant": 0.772689207321,
      "continuity": 0.996589532559
    },
    {
      "r": 95241,
      "seconds": 1587.35,
      "linear_quant": 1.32279166667,
      "sqrt_quant": 0.813262462759,
      "continuity": 0.996759679065
    },
    {
      "r": 105506,
      "seconds": 1758.43333,
      "linear_quant": 1.46536111111,
      "sqrt_quant": 0.855967613614,
      "continuity": 0.996921342184
    },
    {
      "r": 116878,
      "seconds": 1947.96667,
      "linear_quant": 1.62330555556,
      "sqrt_quant": 0.90091774196,
      "continuity": 0.997074947844
    },
    {
      "r": 129475,
      "seconds": 2157.91667,
      "linear_quant": 1.79826388889,
      "sqrt_quant": 0.948225682232,
      "continuity": 0.997220881661
    },
    {
      "r": 143430,
      "seconds": 2390.5,
      "linear_quant": 1.99208333333,
      "sqrt_quant": 0.998018870897,
      "continuity": 0.997359537519
    },
    {
      "r": 158888,
      "seconds": 2648.13333,
      "linear_quant": 2.20677777778,
      "sqrt_quant": 1.05042319514,
      "continuity": 0.997491266953
    },
    {
      "r": 176014,
      "seconds": 2933.56667,
      "linear_quant": 2.44463888889,
      "sqrt_quant": 1.10558556632,
      "continuity": 0.997616438326
    },
    {
      "r": 194984,
      "seconds": 3249.73333,
      "linear_quant": 2.70811111111,
      "sqrt_quant": 1.16363892834,
      "continuity": 0.997735353021
    },
    {
      "r": 216000,
      "seconds": 3600.0,
      "linear_quant": 3.0,
      "sqrt_quant": 1.22474487139,
      "continuity": 0.997848342585
    }
  ],
  "continuity_check": [
    {
      "r": 1,
      "seconds": 0.0166666667,
      "diff_rms": 1.41427756181,
      "theory_diff_rms": 1.41421356237,
      "continuity": 0.0
    },
    {
      "r": 30,
      "seconds": 0.5,
      "diff_rms": 0.258174437206,
      "theory_diff_rms": 0.258198889747,
      "continuity": 0.817451365858
    },
    {
      "r": 60,
      "seconds": 1.0,
      "diff_rms": 0.18258907171,
      "theory_diff_rms": 0.182574185835,
      "continuity": 0.870895871758
    },
    {
      "r": 120,
      "seconds": 2.0,
      "diff_rms": 0.129112616483,
      "theory_diff_rms": 0.129099444874,
      "continuity": 0.908707724728
    },
    {
      "r": 300,
      "seconds": 5.0,
      "diff_rms": 0.0811085763405,
      "theory_diff_rms": 0.0816496580928,
      "continuity": 0.942650170991
    },
    {
      "r": 600,
      "seconds": 10.0,
      "diff_rms": 0.0576247977316,
      "theory_diff_rms": 0.057735026919,
      "continuity": 0.959254958653
    },
    {
      "r": 3600,
      "seconds": 60.0,
      "diff_rms": 0.023429178261,
      "theory_diff_rms": 0.0235702260396,
      "continuity": 0.983433818867
    },
    {
      "r": 21600,
      "seconds": 360.0,
      "diff_rms": 0.00926580720545,
      "theory_diff_rms": 0.00962250448649,
      "continuity": 0.993448381382
    }
  ]
};
