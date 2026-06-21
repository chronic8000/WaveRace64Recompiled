# Wave Race 64 segment map

Parsed from `H:\WaveRace64Recomp\leak-ref\spec` (192 segments).

| Name | VRAM | Flags | Includes | Overlay |
| --- | --- | --- | --- | --- |
| codeSEG | 0x80046800 | BOOT OBJECT | code.o, $(ROOT)/usr/lib/PR/rspboot.o, $(ROOT)/usr/lib/PR/gspF3DEX.fifo.o, … (+9) | no |
| loadCodeSEG | codeSEG | - | dai_anim_data.o, hjm_camera.o, kn_psrestart.o, … (+11) | yes |
| loadAudioTable | 0x80045800 | - | audio/audiotable.o | no |
| zbufferSEG | 0x802A0000 | OBJECT | ot_zbuffer.o | no |
| fbufferSEG | 0x8038F800 | OBJECT | ot_fbuffer.o | no |
| dramStackSEG | 0x80045800 | OBJECT | kn_stack.o | no |
| cameraBuffer | 0x80580000 | OBJECT | hjm_memory.o | no |
| staticSEG | 0x00000001 | OBJECT | ot_ssegment.o, ot_ssletter.o, ot_ssmask.o, … (+2) | no |
| staticGameSEG | staticSEG | OBJECT | sot_sssea.o, sot_ssworld.o, enstatic.o, … (+5) | no |
| nusmarkSEG | "codeSEG" | OBJECT | kn_ssnusmark.o | no |
| staticEngSEG | staticGameSEG | OBJECT | kn_sslanguage.o | no |
| staticGerSEG | staticGameSEG | OBJECT | kn_ssgerman.o | no |
| autoSEG | 0x00000002 | OBJECT | ot_asegment.o | no |
| dynamicSEG | 0x00000003 | OBJECT | ot_dsegment.o | no |
| screenSEG | 0x00000004 | OBJECT | ot_vsegment.o | no |
| endynamic | 0x00000005 | OBJECT | endynamic.o | no |
| sot_dynamic | 0x00000006 | OBJECT | sot_dynamic.o | no |
| knDynamicSEG | 0x00000007 | OBJECT | kn_dynamic.o | no |
| anime | - | OBJECT | sot_anime.o | no |
| endface | - | OBJECT | sot_endface.o | no |
| progEdit | 0x80328000 | OBJECT | enpsedit.o, enpsmachine.o | yes |
| progCPack | 0x802C5800 | OBJECT | kn_pscpack.o | yes |
| progTitle | 0x802C5800 | OBJECT | kn_pstitle.o | yes |
| progRace | 0x802C5800 | OBJECT | kn_psrace.o | yes |
| progMachine | 0x802C5800 | OBJECT | kn_psmachine.o | yes |
| progCTitle | 0x802C5800 | OBJECT | kn_psctitle.o | yes |
| progCourse | 0x802C5800 | OBJECT | kn_pscourse.o | yes |
| progResult | 0x802C5800 | OBJECT | kn_psresult.o, kn_psgameover.o | yes |
| progRecord | 0x802C5800 | OBJECT | kn_psrecord.o | yes |
| progCalc | 0x802C5800 | OBJECT | kn_pscalc.o | yes |
| progVsResult | 0x802C5800 | OBJECT | kn_psvsresult.o | yes |
| progConfig | 0x802C5800 | OBJECT | kn_psconfig.o | yes |
| progCfgName | 0x802C5800 | OBJECT | kn_pscfgname.o | yes |
| progCfgRec | 0x802C5800 | OBJECT | kn_pscfgrec.o | yes |
| progCfgCond | 0x802C5800 | OBJECT | kn_pscfgcond.o | yes |
| progCfgSound | 0x802C5800 | OBJECT | kn_pscfgsound.o | yes |
| progCfgErase | 0x802C5800 | OBJECT | kn_pscfgerase.o | yes |
| progCfgSave | 0x802C5800 | OBJECT | kn_pscfgsave.o | yes |
| progRestart | 0x802C5800 | OBJECT | kn_psrestart2.o | yes |
| progTourEnd | 0x802C5800 | OBJECT | kn_pstourend.o | yes |
| course0 | 0x0000000D | OBJECT | ensscrs0.o, ensscrs0_zh.o | no |
| course1 | 0x0000000D | OBJECT | ensscrs1.o, ensscrs1_zh.o | no |
| course2 | 0x0000000D | OBJECT | ensscrs2.o, ensscrs2_zh.o | no |
| course3 | 0x0000000D | OBJECT | ensscrs3.o, ensscrs3_zh.o | no |
| course4 | 0x0000000D | OBJECT | ensscrs4.o, ensscrs4_zh.o | no |
| course5 | 0x0000000D | OBJECT | ensscrs5.o, ensscrs5_zh.o | no |
| course6 | 0x0000000D | OBJECT | ensscrs6.o, ensscrs6_zh.o | no |
| course7 | 0x0000000D | OBJECT | ensscrs7.o, ensscrs7_zh.o | no |
| course8 | 0x0000000D | OBJECT | ensscrs8.o, ensscrs8_zh.o | no |
| course7_end | 0x0000000D | OBJECT | ensscrs7_end.o, ensscrs7_end_zh.o | no |
| course1_2p | 0x0000000D | OBJECT | ensscrs1_2p.o, ensscrs1_2p_zh.o | no |
| course2_2p | 0x0000000D | OBJECT | ensscrs2_2p.o, ensscrs2_2p_zh.o | no |
| course3_2p | 0x0000000D | OBJECT | ensscrs3_2p.o, ensscrs3_2p_zh.o | no |
| course4_2p | 0x0000000D | OBJECT | ensscrs4_2p.o, ensscrs4_2p_zh.o | no |
| course5_2p | 0x0000000D | OBJECT | ensscrs5_2p.o, ensscrs5_2p_zh.o | no |
| course6_2p | 0x0000000D | OBJECT | ensscrs6_2p.o, ensscrs6_2p_zh.o | no |
| course7_2p | 0x0000000D | OBJECT | ensscrs7_2p.o, ensscrs7_2p_zh.o | no |
| course8_2p | 0x0000000D | OBJECT | ensscrs8_2p.o, ensscrs8_2p_zh.o | no |
| course1_com | 0x0000000E | OBJECT | ensscom1.o, ensscom1_zh.o | no |
| course2_com | 0x0000000E | OBJECT | ensscom2.o, ensscom2_zh.o | no |
| course3_com | 0x0000000E | OBJECT | ensscom3_zh.o, ensscom3.o | no |
| course4_com | 0x0000000E | OBJECT | ensscom4_zh.o, ensscom4.o | no |
| course5_com | 0x0000000E | OBJECT | ensscom5_zh.o, ensscom5.o | no |
| course6_com | 0x0000000E | OBJECT | ensscom6_zh.o, ensscom6.o | no |
| course7_com | 0x0000000E | OBJECT | ensscom7_zh.o, ensscom7.o | no |
| course8_com | 0x0000000E | OBJECT | ensscom8_zh.o, ensscom8.o | no |
| start_gate1 | 0x8029A200 | OBJECT | enssstart1_zh.o, enssstart1.o | no |
| start_gate2 | 0x8029A200 | OBJECT | enssstart2_zh.o, enssstart2.o | no |
| start_gate7 | 0x8029A200 | OBJECT | enssstart7_zh.o, enssstart7.o | no |
| course0DT | 0x80306800 | OBJECT | endtcrs0.o, endtsca0.o, endtdemo0.o | no |
| course1DT | 0x80306800 | OBJECT | endtcrs1.o, endtsca1.o, endtdemo1.o, … (+1) | no |
| course2DT | 0x80306800 | OBJECT | endtcrs2.o, endtsca2.o, endtdemo2.o, … (+1) | no |
| course3DT | 0x80306800 | OBJECT | endtcrs3.o, endtsca3.o, endtdemo3.o, … (+1) | no |
| course4DT | 0x80306800 | OBJECT | endtcrs4.o, endtsca4.o, endtdemo4.o, … (+1) | no |
| course5DT | 0x80306800 | OBJECT | endtcrs5.o, endtsca5.o, endtdemo5.o, … (+1) | no |
| course6DT | 0x80306800 | OBJECT | endtcrs6.o, endtsca6.o, endtdemo6.o, … (+1) | no |
| course7DT | 0x80306800 | OBJECT | endtcrs7.o, endtsca7.o, endtdemo7.o, … (+1) | no |
| course8DT | 0x80306800 | OBJECT | endtcrs8.o, endtsca8.o, endtdemo8.o, … (+1) | no |
| course2DT_2p | 0x80306800 | OBJECT | endtcrs2_2p.o | no |
| course5DT_2p | 0x80306800 | OBJECT | endtcrs5_2p.o | no |
| bankCPackSEG | 0x00000008 | OBJECT | kn_sscpack.o, kn_sscpack_zh.o | no |
| bankCPackEfSEG | 0x00000008 | OBJECT | kn_sscpack_e.o | no |
| bankCPackGeSEG | 0x00000008 | OBJECT | kn_sscpack_g.o | no |
| bankTitleSEG | 0x00000008 | OBJECT | kn_sstitle.o, kn_sstitle_zh.o | no |
| bankTitleGerSEG | 0x00000008 | OBJECT | kn_sstitle_ger.o | no |
| bankRaceSEG | 0x00000008 | OBJECT | kn_ssrace.o, kn_ssrace_zh.o | no |
| bankRace2pSEG | 0x00000008 | OBJECT | kn_ssrace2p_zh.o, kn_ssrace2p.o | no |
| bankRetireSEG | 0x00000008 | OBJECT | kn_ssretire.o, kn_ssretire_zh.o | no |
| bankGoalSEG | 0x00000008 | OBJECT | kn_ssgoal.o, kn_ssgoal_zh.o | no |
| bankNewRecordSEG | 0x00000008 | OBJECT | kn_ssnewrecord.o, kn_ssnewrecord_zh.o | no |
| bankParkSEG | 0x00000008 | OBJECT | kn_sspark.o, kn_sspark_zh.o | no |
| bankParkGerSEG | 0x00000008 | OBJECT | kn_sspark_ger.o | no |
| bankTimeUpSEG | 0x00000008 | OBJECT | kn_sstimeup.o, kn_sstimeup_zh.o | no |
| bankGameOverSEG | 0x00000008 | OBJECT | kn_ssgameover.o | no |
| bankMachineSEG | 0x00000008 | OBJECT | kn_ssmachine.o, kn_ssmachine_zh.o | no |
| bankCourseSEG | 0x00000008 | OBJECT | kn_sscourse.o, kn_sscourse_zh.o | no |
| bankResultSEG | 0x00000008 | OBJECT | kn_ssresult.o | no |
| bankCalcSEG | 0x00000008 | OBJECT | kn_sscalc.o, kn_sscalc_zh.o | no |
| bankVsResultSEG | 0x00000008 | OBJECT | kn_ssvsresult.o, kn_ssvsresult_zh.o | no |
| bankCTitleSEG | 0x00000008 | OBJECT | kn_ssctitle.o, kn_ssctitle_zh.o | no |
| bankConfigSEG | 0x00000008 | OBJECT | kn_ssconfig.o, kn_ssconfig_zh.o | no |
| bankCfgNameSEG | 0x00000008 | OBJECT | kn_sscfgname.o, kn_sscfgname_zh.o | no |
| bankCfgRecSEG | 0x00000008 | OBJECT | kn_sscfgrec.o, kn_sscfgrec_zh.o | no |
| bankCfgCondSEG | 0x00000008 | OBJECT | kn_sscfgcond.o, kn_sscfgcond_zh.o | no |
| bankCfgSoundSEG | 0x00000008 | OBJECT | kn_sscfgsound.o, kn_sscfgsound_zh.o | no |
| bankCfgEraseSEG | 0x00000008 | OBJECT | kn_sscfgerase.o, kn_sscfgerase_zh.o | no |
| bankCfgSaveSEG | 0x00000008 | OBJECT | kn_sscfgsave.o, kn_sscfgsave_zh.o | no |
| bankPfsMesSEG | 0x00000008 | OBJECT | kn_sspfsmes.o, kn_sspfsmes_zh.o | no |
| bankPfsMesGhostSEG | 0x00000008 | OBJECT | kn_sspfsmes_ghost.o, kn_sspfsmes_ghost_zh.o | no |
| bankTourEndSEG | 0x00000008 | OBJECT | kn_sstourend.o, kn_sstourend_zh.o | no |
| bankEditSEG | 0x00000008 | OBJECT | enssedit.o | no |
| bankLetter | 0x00000008 | OBJECT | kn_ssletter.o, kn_ssletter_zh.o | no |
| bankCMap1SEG | 0x00000008 | OBJECT | hjm_sscourse_map1.o | no |
| bankCMap2SEG | 0x00000008 | OBJECT | hjm_sscourse_map2.o | no |
| bankCMap3SEG | 0x00000008 | OBJECT | hjm_sscourse_map3.o | no |
| bankCMap4SEG | 0x00000008 | OBJECT | hjm_sscourse_map4.o | no |
| bankCMap5SEG | 0x00000008 | OBJECT | hjm_sscourse_map5.o | no |
| bankCMap6SEG | 0x00000008 | OBJECT | hjm_sscourse_map6.o | no |
| bankCMap7SEG | 0x00000008 | OBJECT | hjm_sscourse_map7.o | no |
| bankCMap8SEG | 0x00000008 | OBJECT | hjm_sscourse_map8.o | no |
| bankCMapDSEG | 0x00000008 | OBJECT | hjm_sscourse_mapD.o | no |
| bankBackFine1SEG | 0x00000008 | OBJECT | sot_back_fine1.o | no |
| bankBackEveSEG | 0x00000008 | OBJECT | sot_back_eve.o | no |
| bankBackStoSEG | 0x00000008 | OBJECT | sot_back_sto.o | no |
| bankBackMornSEG | 0x00000008 | OBJECT | sot_back_morn.o | no |
| bankBackDirtSEG | 0x00000008 | OBJECT | sot_back_dirt.o | no |
| bankBackNightSEG | 0x00000008 | OBJECT | sot_back_night.o | no |
| bankBackFine2SEG | 0x00000008 | OBJECT | sot_back_fine2.o | no |
| bankSeaFine1SEG | 0x00000008 | OBJECT | sot_sea_fine1.o | no |
| bankSeaEveSEG | 0x00000008 | OBJECT | sot_sea_eve.o | no |
| bankSeaStoSEG | 0x00000008 | OBJECT | sot_sea_sto.o | no |
| bankSeaMornSEG | 0x00000008 | OBJECT | sot_sea_morn.o | no |
| bankSeaDirtSEG | 0x00000008 | OBJECT | sot_sea_dirt.o | no |
| bankSeaNightSEG | 0x00000008 | OBJECT | sot_sea_night.o | no |
| bankSeaFine2SEG | 0x00000008 | OBJECT | sot_sea_fine2.o | no |
| bankSeaIceSEG | 0x00000008 | OBJECT | sot_sea_ice.o | no |
| bankSeaFine1_2pSEG | 0x00000008 | OBJECT | sot_sea_fine1_2p.o | no |
| bankSeaEve_2pSEG | 0x00000008 | OBJECT | sot_sea_eve_2p.o | no |
| bankSeaSto_2pSEG | 0x00000008 | OBJECT | sot_sea_sto_2p.o | no |
| bankSeaMorn_2pSEG | 0x00000008 | OBJECT | sot_sea_morn_2p.o | no |
| bankSeaDirt_2pSEG | 0x00000008 | OBJECT | sot_sea_dirt_2p.o | no |
| bankSeaNight_2pSEG | 0x00000008 | OBJECT | sot_sea_night_2p.o | no |
| bankSeaFine2_2pSEG | 0x00000008 | OBJECT | sot_sea_fine2_2p.o | no |
| bankSeaIce_2pSEG | 0x00000008 | OBJECT | sot_sea_ice_2p.o | no |
| bankBottomFine1SEG | 0x00000008 | OBJECT | sot_bottom_fine1.o | no |
| bankBottomEveSEG | 0x00000008 | OBJECT | sot_bottom_eve.o | no |
| bankBottomStoSEG | 0x00000008 | OBJECT | sot_bottom_sto.o | no |
| bankBottomMornSEG | 0x00000008 | OBJECT | sot_bottom_morn.o | no |
| bankBottomDirtSEG | 0x00000008 | OBJECT | sot_bottom_dirt.o | no |
| bankBottomNightSEG | 0x00000008 | OBJECT | sot_bottom_night.o | no |
| bankBottomFine2SEG | 0x00000008 | OBJECT | sot_bottom_fine2.o | no |
| bankCloFine1SEG | 0x00000008 | OBJECT | sot_clo_fine1.o | no |
| bankCloEveSEG | 0x00000008 | OBJECT | sot_clo_eve.o | no |
| bankCloStoSEG | 0x00000008 | OBJECT | sot_clo_sto.o | no |
| bankCloMornSEG | 0x00000008 | OBJECT | sot_clo_morn.o | no |
| bankCloDirtSEG | 0x00000008 | OBJECT | sot_clo_dirt.o | no |
| bankCloNightSEG | 0x00000008 | OBJECT | sot_clo_night.o | no |
| bankCloFine2SEG | 0x00000008 | OBJECT | sot_clo_fine2.o | no |
| bankSunDolSEG | 0x00000008 | OBJECT | sot_sun_dol.o | no |
| bankSunEveSEG | 0x00000008 | OBJECT | sot_sun.o | no |
| bankJetSki0SEG | 0x00000008 | OBJECT | dai_ssJetSki0_zh.o, dai_ssJetSki0.o | no |
| bankJetSki0bSEG | 0x00000008 | OBJECT | dai_ssJetSki0b_zh.o, dai_ssJetSki0b.o | no |
| bankJetSki1SEG | 0x00000008 | OBJECT | dai_ssJetSki1_zh.o, dai_ssJetSki1.o | no |
| bankJetSki1bSEG | 0x00000008 | OBJECT | dai_ssJetSki1b_zh.o, dai_ssJetSki1b.o | no |
| bankJetSki2SEG | 0x00000008 | OBJECT | dai_ssJetSki2_zh.o, dai_ssJetSki2.o | no |
| bankJetSki2bSEG | 0x00000008 | OBJECT | dai_ssJetSki2b_zh.o, dai_ssJetSki2b.o | no |
| bankJetSki3SEG | 0x00000008 | OBJECT | dai_ssJetSki3_zh.o, dai_ssJetSki3.o | no |
| bankJetSki3bSEG | 0x00000008 | OBJECT | dai_ssJetSki3b_zh.o, dai_ssJetSki3b.o | no |
| bankJetSki4SEG | 0x00000008 | OBJECT | dai_ssJetSki4_zh.o, dai_ssJetSki4.o | no |
| bankJetSki4bSEG | 0x00000008 | OBJECT | dai_ssJetSki4b_zh.o, dai_ssJetSki4b.o | no |
| bankJetSki5SEG | 0x00000008 | OBJECT | dai_ssJetSki5_zh.o, dai_ssJetSki5.o | no |
| bankJetSki5bSEG | 0x00000008 | OBJECT | dai_ssJetSki5b_zh.o, dai_ssJetSki5b.o | no |
| bankJetSki6SEG | 0x00000008 | OBJECT | dai_ssJetSki6_zh.o, dai_ssJetSki6.o | no |
| bankJetSki6bSEG | 0x00000008 | OBJECT | dai_ssJetSki6b_zh.o, dai_ssJetSki6b.o | no |
| bankJetSki7SEG | 0x00000008 | OBJECT | dai_ssJetSki7_zh.o, dai_ssJetSki7.o | no |
| bankJetSki7bSEG | 0x00000008 | OBJECT | dai_ssJetSki7b_zh.o, dai_ssJetSki7b.o | no |
| bankDolphinSEG | 0x00000008 | OBJECT | dai_dolphin.o | no |
| bankDolphinRedSEG | 0x00000008 | OBJECT | dai_dolphin_r.o | no |
| bankOrcaSEG | 0x00000008 | OBJECT | dai_orca.o | no |
| bankFishSEG | 0x00000008 | OBJECT | dai_fish.o | no |
| bankSeagullSEG | 0x00000008 | OBJECT | dai_seagull.o | no |
| bankTroutSEG | 0x00000008 | OBJECT | dai_trout.o | no |
| bankFlyfishSEG | 0x00000008 | OBJECT | dai_flyfish.o | no |
| bankDuckSEG | 0x00000008 | OBJECT | dai_duck.o | no |
| bankHelicopterSEG | 0x00000008 | OBJECT | dai_helicopter.o | no |
| bankPenguinSEG | 0x00000008 | OBJECT | dai_penguin.o | no |
| bankEndShapeSEG | 0x00000008 | OBJECT | dai_end_s.o | no |
| bankensssca | 0x00000008 | OBJECT | ensssca_zh.o, ensssca.o | no |
| Audiobank | - | RAW | audio/wr64_audio.banks.cart, audio/zh/wr64_audio.banks.cart | no |
| Audiotable | - | RAW | audio/wr64_audio.table.cart, audio/zh/wr64_audio.table.cart | no |
| Audioseq | - | RAW | audio/wr64_audio.music.cart, audio/zh/wr64_audio.music.cart | no |
| Buffers | 0x80001000 | OBJECT | audio/audioheap.o, audio/audiowork.o | no |
