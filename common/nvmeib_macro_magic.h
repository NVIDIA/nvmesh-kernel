/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_MACRO_MAGIC_H
#define NVMEIB_MACRO_MAGIC_H

/*=============================================================================
    Copyright (c) 2015 Paul Fultz II
    cloak.h
    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/

#define STRINGIFY(s) XSTRINGIFY(s)
#define XSTRINGIFY(s) #s

#define CAT(a, ...) PRIMITIVE_CAT(a, __VA_ARGS__)
#define PRIMITIVE_CAT(a, ...) a ## __VA_ARGS__

#define COMPL(b) PRIMITIVE_CAT(COMPL_, b)
#define COMPL_0 1
#define COMPL_1 0

#define BITAND(x) PRIMITIVE_CAT(BITAND_, x)
#define BITAND_0(y) 0
#define BITAND_1(y) y

#define INC(x) PRIMITIVE_CAT(INC_, x)
#define INC_0 1
#define INC_1 2
#define INC_2 3
#define INC_3 4
#define INC_4 5
#define INC_5 6
#define INC_6 7
#define INC_7 8
#define INC_8 9
#define INC_9 10
#define INC_10 11
#define INC_11 12
#define INC_12 13
#define INC_13 14
#define INC_14 15
#define INC_15 16
#define INC_16 17
#define INC_17 18
#define INC_18 19
#define INC_19 20
#define INC_20 21
#define INC_21 22
#define INC_22 23
#define INC_23 24
#define INC_24 25
#define INC_25 26
#define INC_26 27
#define INC_27 28
#define INC_28 29
#define INC_29 30
#define INC_30 31
#define INC_31 32
#define INC_32 33
#define INC_33 34
#define INC_34 35
#define INC_35 36
#define INC_36 37
#define INC_37 38
#define INC_38 39
#define INC_39 40
#define INC_40 41
#define INC_41 42
#define INC_42 43
#define INC_43 44
#define INC_44 45
#define INC_45 46
#define INC_46 47
#define INC_47 48
#define INC_48 49
#define INC_49 50
#define INC_50 51
#define INC_51 52
#define INC_52 53
#define INC_53 54
#define INC_54 55
#define INC_55 56
#define INC_56 57
#define INC_57 58
#define INC_58 59
#define INC_59 60
#define INC_60 61
#define INC_61 62
#define INC_62 63
#define INC_63 64
#define INC_64 65
#define INC_65 66
#define INC_66 67
#define INC_67 68
#define INC_68 69
#define INC_69 70
#define INC_70 71
#define INC_71 72
#define INC_72 73
#define INC_73 74
#define INC_74 75
#define INC_75 76
#define INC_76 77
#define INC_77 78
#define INC_78 79
#define INC_79 80
#define INC_80 81
#define INC_81 82
#define INC_82 83
#define INC_83 84
#define INC_84 85
#define INC_85 86
#define INC_86 87
#define INC_87 88
#define INC_88 89
#define INC_89 90
#define INC_90 91
#define INC_91 92
#define INC_92 93
#define INC_93 94
#define INC_94 95
#define INC_95 96
#define INC_96 97
#define INC_97 98
#define INC_98 99
#define INC_99 100
#define INC_100 101
#define INC_101 102
#define INC_102 103
#define INC_103 104
#define INC_104 105
#define INC_105 106
#define INC_106 107
#define INC_107 108
#define INC_108 109
#define INC_109 110
#define INC_110 111
#define INC_111 112
#define INC_112 113
#define INC_113 114
#define INC_114 115
#define INC_115 116
#define INC_116 117
#define INC_117 118
#define INC_118 119
#define INC_119 120
#define INC_120 121
#define INC_121 122
#define INC_122 123
#define INC_123 124
#define INC_124 125
#define INC_125 126
#define INC_126 127
#define INC_127 128
#define INC_128 129

#define DEC(x) PRIMITIVE_CAT(DEC_, x)
// Below list was created by bash cmd: for i in {0..513}; do echo "#define DEC_${i} $((${i}-1))"; done;
#define DEC_0 0
#define DEC_1 0
#define DEC_2 1
#define DEC_3 2
#define DEC_4 3
#define DEC_5 4
#define DEC_6 5
#define DEC_7 6
#define DEC_8 7
#define DEC_9 8
#define DEC_10 9
#define DEC_11 10
#define DEC_12 11
#define DEC_13 12
#define DEC_14 13
#define DEC_15 14
#define DEC_16 15
#define DEC_17 16
#define DEC_18 17
#define DEC_19 18
#define DEC_20 19
#define DEC_21 20
#define DEC_22 21
#define DEC_23 22
#define DEC_24 23
#define DEC_25 24
#define DEC_26 25
#define DEC_27 26
#define DEC_28 27
#define DEC_29 28
#define DEC_30 29
#define DEC_31 30
#define DEC_32 31
#define DEC_33 32
#define DEC_34 33
#define DEC_35 34
#define DEC_36 35
#define DEC_37 36
#define DEC_38 37
#define DEC_39 38
#define DEC_40 39
#define DEC_41 40
#define DEC_42 41
#define DEC_43 42
#define DEC_44 43
#define DEC_45 44
#define DEC_46 45
#define DEC_47 46
#define DEC_48 47
#define DEC_49 48
#define DEC_50 49
#define DEC_51 50
#define DEC_52 51
#define DEC_53 52
#define DEC_54 53
#define DEC_55 54
#define DEC_56 55
#define DEC_57 56
#define DEC_58 57
#define DEC_59 58
#define DEC_60 59
#define DEC_61 60
#define DEC_62 61
#define DEC_63 62
#define DEC_64 63
#define DEC_65 64
#define DEC_66 65
#define DEC_67 66
#define DEC_68 67
#define DEC_69 68
#define DEC_70 69
#define DEC_71 70
#define DEC_72 71
#define DEC_73 72
#define DEC_74 73
#define DEC_75 74
#define DEC_76 75
#define DEC_77 76
#define DEC_78 77
#define DEC_79 78
#define DEC_80 79
#define DEC_81 80
#define DEC_82 81
#define DEC_83 82
#define DEC_84 83
#define DEC_85 84
#define DEC_86 85
#define DEC_87 86
#define DEC_88 87
#define DEC_89 88
#define DEC_90 89
#define DEC_91 90
#define DEC_92 91
#define DEC_93 92
#define DEC_94 93
#define DEC_95 94
#define DEC_96 95
#define DEC_97 96
#define DEC_98 97
#define DEC_99 98
#define DEC_100 99
#define DEC_101 100
#define DEC_102 101
#define DEC_103 102
#define DEC_104 103
#define DEC_105 104
#define DEC_106 105
#define DEC_107 106
#define DEC_108 107
#define DEC_109 108
#define DEC_110 109
#define DEC_111 110
#define DEC_112 111
#define DEC_113 112
#define DEC_114 113
#define DEC_115 114
#define DEC_116 115
#define DEC_117 116
#define DEC_118 117
#define DEC_119 118
#define DEC_120 119
#define DEC_121 120
#define DEC_122 121
#define DEC_123 122
#define DEC_124 123
#define DEC_125 124
#define DEC_126 125
#define DEC_127 126
#define DEC_128 127
#define DEC_129 128
#define DEC_130 129
#define DEC_131 130
#define DEC_132 131
#define DEC_133 132
#define DEC_134 133
#define DEC_135 134
#define DEC_136 135
#define DEC_137 136
#define DEC_138 137
#define DEC_139 138
#define DEC_140 139
#define DEC_141 140
#define DEC_142 141
#define DEC_143 142
#define DEC_144 143
#define DEC_145 144
#define DEC_146 145
#define DEC_147 146
#define DEC_148 147
#define DEC_149 148
#define DEC_150 149
#define DEC_151 150
#define DEC_152 151
#define DEC_153 152
#define DEC_154 153
#define DEC_155 154
#define DEC_156 155
#define DEC_157 156
#define DEC_158 157
#define DEC_159 158
#define DEC_160 159
#define DEC_161 160
#define DEC_162 161
#define DEC_163 162
#define DEC_164 163
#define DEC_165 164
#define DEC_166 165
#define DEC_167 166
#define DEC_168 167
#define DEC_169 168
#define DEC_170 169
#define DEC_171 170
#define DEC_172 171
#define DEC_173 172
#define DEC_174 173
#define DEC_175 174
#define DEC_176 175
#define DEC_177 176
#define DEC_178 177
#define DEC_179 178
#define DEC_180 179
#define DEC_181 180
#define DEC_182 181
#define DEC_183 182
#define DEC_184 183
#define DEC_185 184
#define DEC_186 185
#define DEC_187 186
#define DEC_188 187
#define DEC_189 188
#define DEC_190 189
#define DEC_191 190
#define DEC_192 191
#define DEC_193 192
#define DEC_194 193
#define DEC_195 194
#define DEC_196 195
#define DEC_197 196
#define DEC_198 197
#define DEC_199 198
#define DEC_200 199
#define DEC_201 200
#define DEC_202 201
#define DEC_203 202
#define DEC_204 203
#define DEC_205 204
#define DEC_206 205
#define DEC_207 206
#define DEC_208 207
#define DEC_209 208
#define DEC_210 209
#define DEC_211 210
#define DEC_212 211
#define DEC_213 212
#define DEC_214 213
#define DEC_215 214
#define DEC_216 215
#define DEC_217 216
#define DEC_218 217
#define DEC_219 218
#define DEC_220 219
#define DEC_221 220
#define DEC_222 221
#define DEC_223 222
#define DEC_224 223
#define DEC_225 224
#define DEC_226 225
#define DEC_227 226
#define DEC_228 227
#define DEC_229 228
#define DEC_230 229
#define DEC_231 230
#define DEC_232 231
#define DEC_233 232
#define DEC_234 233
#define DEC_235 234
#define DEC_236 235
#define DEC_237 236
#define DEC_238 237
#define DEC_239 238
#define DEC_240 239
#define DEC_241 240
#define DEC_242 241
#define DEC_243 242
#define DEC_244 243
#define DEC_245 244
#define DEC_246 245
#define DEC_247 246
#define DEC_248 247
#define DEC_249 248
#define DEC_250 249
#define DEC_251 250
#define DEC_252 251
#define DEC_253 252
#define DEC_254 253
#define DEC_255 254
#define DEC_256 255
#define DEC_257 256
#define DEC_258 257
#define DEC_259 258
#define DEC_260 259
#define DEC_261 260
#define DEC_262 261
#define DEC_263 262
#define DEC_264 263
#define DEC_265 264
#define DEC_266 265
#define DEC_267 266
#define DEC_268 267
#define DEC_269 268
#define DEC_270 269
#define DEC_271 270
#define DEC_272 271
#define DEC_273 272
#define DEC_274 273
#define DEC_275 274
#define DEC_276 275
#define DEC_277 276
#define DEC_278 277
#define DEC_279 278
#define DEC_280 279
#define DEC_281 280
#define DEC_282 281
#define DEC_283 282
#define DEC_284 283
#define DEC_285 284
#define DEC_286 285
#define DEC_287 286
#define DEC_288 287
#define DEC_289 288
#define DEC_290 289
#define DEC_291 290
#define DEC_292 291
#define DEC_293 292
#define DEC_294 293
#define DEC_295 294
#define DEC_296 295
#define DEC_297 296
#define DEC_298 297
#define DEC_299 298
#define DEC_300 299
#define DEC_301 300
#define DEC_302 301
#define DEC_303 302
#define DEC_304 303
#define DEC_305 304
#define DEC_306 305
#define DEC_307 306
#define DEC_308 307
#define DEC_309 308
#define DEC_310 309
#define DEC_311 310
#define DEC_312 311
#define DEC_313 312
#define DEC_314 313
#define DEC_315 314
#define DEC_316 315
#define DEC_317 316
#define DEC_318 317
#define DEC_319 318
#define DEC_320 319
#define DEC_321 320
#define DEC_322 321
#define DEC_323 322
#define DEC_324 323
#define DEC_325 324
#define DEC_326 325
#define DEC_327 326
#define DEC_328 327
#define DEC_329 328
#define DEC_330 329
#define DEC_331 330
#define DEC_332 331
#define DEC_333 332
#define DEC_334 333
#define DEC_335 334
#define DEC_336 335
#define DEC_337 336
#define DEC_338 337
#define DEC_339 338
#define DEC_340 339
#define DEC_341 340
#define DEC_342 341
#define DEC_343 342
#define DEC_344 343
#define DEC_345 344
#define DEC_346 345
#define DEC_347 346
#define DEC_348 347
#define DEC_349 348
#define DEC_350 349
#define DEC_351 350
#define DEC_352 351
#define DEC_353 352
#define DEC_354 353
#define DEC_355 354
#define DEC_356 355
#define DEC_357 356
#define DEC_358 357
#define DEC_359 358
#define DEC_360 359
#define DEC_361 360
#define DEC_362 361
#define DEC_363 362
#define DEC_364 363
#define DEC_365 364
#define DEC_366 365
#define DEC_367 366
#define DEC_368 367
#define DEC_369 368
#define DEC_370 369
#define DEC_371 370
#define DEC_372 371
#define DEC_373 372
#define DEC_374 373
#define DEC_375 374
#define DEC_376 375
#define DEC_377 376
#define DEC_378 377
#define DEC_379 378
#define DEC_380 379
#define DEC_381 380
#define DEC_382 381
#define DEC_383 382
#define DEC_384 383
#define DEC_385 384
#define DEC_386 385
#define DEC_387 386
#define DEC_388 387
#define DEC_389 388
#define DEC_390 389
#define DEC_391 390
#define DEC_392 391
#define DEC_393 392
#define DEC_394 393
#define DEC_395 394
#define DEC_396 395
#define DEC_397 396
#define DEC_398 397
#define DEC_399 398
#define DEC_400 399
#define DEC_401 400
#define DEC_402 401
#define DEC_403 402
#define DEC_404 403
#define DEC_405 404
#define DEC_406 405
#define DEC_407 406
#define DEC_408 407
#define DEC_409 408
#define DEC_410 409
#define DEC_411 410
#define DEC_412 411
#define DEC_413 412
#define DEC_414 413
#define DEC_415 414
#define DEC_416 415
#define DEC_417 416
#define DEC_418 417
#define DEC_419 418
#define DEC_420 419
#define DEC_421 420
#define DEC_422 421
#define DEC_423 422
#define DEC_424 423
#define DEC_425 424
#define DEC_426 425
#define DEC_427 426
#define DEC_428 427
#define DEC_429 428
#define DEC_430 429
#define DEC_431 430
#define DEC_432 431
#define DEC_433 432
#define DEC_434 433
#define DEC_435 434
#define DEC_436 435
#define DEC_437 436
#define DEC_438 437
#define DEC_439 438
#define DEC_440 439
#define DEC_441 440
#define DEC_442 441
#define DEC_443 442
#define DEC_444 443
#define DEC_445 444
#define DEC_446 445
#define DEC_447 446
#define DEC_448 447
#define DEC_449 448
#define DEC_450 449
#define DEC_451 450
#define DEC_452 451
#define DEC_453 452
#define DEC_454 453
#define DEC_455 454
#define DEC_456 455
#define DEC_457 456
#define DEC_458 457
#define DEC_459 458
#define DEC_460 459
#define DEC_461 460
#define DEC_462 461
#define DEC_463 462
#define DEC_464 463
#define DEC_465 464
#define DEC_466 465
#define DEC_467 466
#define DEC_468 467
#define DEC_469 468
#define DEC_470 469
#define DEC_471 470
#define DEC_472 471
#define DEC_473 472
#define DEC_474 473
#define DEC_475 474
#define DEC_476 475
#define DEC_477 476
#define DEC_478 477
#define DEC_479 478
#define DEC_480 479
#define DEC_481 480
#define DEC_482 481
#define DEC_483 482
#define DEC_484 483
#define DEC_485 484
#define DEC_486 485
#define DEC_487 486
#define DEC_488 487
#define DEC_489 488
#define DEC_490 489
#define DEC_491 490
#define DEC_492 491
#define DEC_493 492
#define DEC_494 493
#define DEC_495 494
#define DEC_496 495
#define DEC_497 496
#define DEC_498 497
#define DEC_499 498
#define DEC_500 499
#define DEC_501 500
#define DEC_502 501
#define DEC_503 502
#define DEC_504 503
#define DEC_505 504
#define DEC_506 505
#define DEC_507 506
#define DEC_508 507
#define DEC_509 508
#define DEC_510 509
#define DEC_511 510
#define DEC_512 511
#define DEC_513 512

#define CHECK_N(x, n, ...) n
#define CHECK(...) CHECK_N(__VA_ARGS__, 0,)
#define PROBE(x) x, 1,

#define IS_PAREN(x) CHECK(IS_PAREN_PROBE x)
#define IS_PAREN_PROBE(...) PROBE(~)

#define NOT(x) CHECK(PRIMITIVE_CAT(NOT_, x))
#define NOT_0 PROBE(~)

#define COMPL(b) PRIMITIVE_CAT(COMPL_, b)
#define COMPL_0 1
#define COMPL_1 0

#define BOOL(x) COMPL(NOT(x))

#define IIF(c) PRIMITIVE_CAT(IIF_, c)
#define IIF_0(t, ...) __VA_ARGS__
#define IIF_1(t, ...) t

#define IF(c) IIF(BOOL(c))

#define EAT(...)
#define EXPAND(...) __VA_ARGS__
#define WHEN(c) IF(c)(EXPAND, EAT)

#define EMPTY()
#define DEFER(id) id EMPTY()
#define OBSTRUCT(id) id DEFER(EMPTY)()

#define EVAL(...)  EVAL1(EVAL1(EVAL1(__VA_ARGS__)))
#define EVAL1(...) EVAL2(EVAL2(EVAL2(__VA_ARGS__)))
#define EVAL2(...) EVAL3(EVAL3(EVAL3(__VA_ARGS__)))
#define EVAL3(...) EVAL4(EVAL4(EVAL4(__VA_ARGS__)))
#define EVAL4(...) EVAL5(EVAL5(EVAL5(__VA_ARGS__)))
#define EVAL5(...) __VA_ARGS__

#define REPEAT(count, macro, ...) \
    WHEN(count) \
    ( \
        OBSTRUCT(REPEAT_INDIRECT) () \
        ( \
            DEC(count), macro, __VA_ARGS__ \
        ) \
        OBSTRUCT(macro) \
        ( \
            DEC(count), __VA_ARGS__ \
        ) \
    )
#define REPEAT_INDIRECT() REPEAT

#define WHILE(pred, op, ...) \
    IF(pred(__VA_ARGS__)) \
    ( \
        OBSTRUCT(WHILE_INDIRECT) () \
        ( \
            pred, op, op(__VA_ARGS__) \
        ), \
        __VA_ARGS__ \
    )
#define WHILE_INDIRECT() WHILE

#define PRIMITIVE_COMPARE(x, y) IS_PAREN \
( \
    COMPARE_ ## x ( COMPARE_ ## y) (())  \
)

#define IS_COMPARABLE(x) IS_PAREN( CAT(COMPARE_, x) (()) )

#define NOT_EQUAL(x, y) \
IIF(BITAND(IS_COMPARABLE(x))(IS_COMPARABLE(y)) ) \
( \
   PRIMITIVE_COMPARE, \
   1 EAT \
)(x, y)

#define EQUAL(x, y) COMPL(NOT_EQUAL(x, y))

#define NV_COMMA() ,

#define COMMA_IF(n) IF(n)(NV_COMMA, EAT)()

#endif
