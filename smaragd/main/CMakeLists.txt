# Smaragd application (proposal 14). The app is LAYERED (Phase 6):
#
#     app_model < app_core < app_objects < app_ui
#
# and since this file, the layers are COMPILE-TIME ENFORCED: each layer is
# an OBJECT library that publishes only its own modules' include dirs and
# links only the lower layers plus its declared engine modules — a
# cross-layer include (model -> actions, core -> objects, anything below the
# UI -> shell) fails to compile. Intra-layer module edges are still guarded
# by tools/check_layering.py.
#
# OBJECT (not STATIC) libraries are load-bearing: the actions and the
# loader/editor/extern-file registries self-register via static
# initializers, which STATIC libraries would silently drop at link time.
# Consequence: the executables must link ALL FOUR layer targets directly
# (object files do not propagate transitively through object libraries).

set(APP_MODEL_FILES
    # --- app/model
    model/include/app/model/sarrangements.h
    model/include/app/model/sappcontext.h
    model/include/app/model/sautomationlane.h
    model/include/app/model/sclipwindow.h
    model/include/app/model/sdetaileditors.h
    model/include/app/model/sexternfile.h
    model/include/app/model/sexternfilelist.h
    model/include/app/model/sfilepathref.h
    model/include/app/model/slink.h
    model/include/app/model/sobject.h
    model/include/app/model/sobjectpath.h
    model/include/app/model/sobjectrenderer.h
    model/include/app/model/splacements.h
    model/include/app/model/sproject.h
    model/include/app/model/seditgroups.h
    model/include/app/model/sprojectprops.h
    model/include/app/model/ssolorules.h
    model/include/app/model/ssortedobjlist.h
    model/src/sappcontext.cpp
    model/src/sautomationlane.cpp
    model/src/sclipwindow.cpp
    model/src/sdetaileditors.cpp
    model/src/sexternfile.cpp
    model/src/sexternfilelist.cpp
    model/src/sfilepathref.cpp
    model/src/slink.cpp
    model/src/sobject.cpp
    model/src/sobjectrenderer.cpp
    model/src/sproject.cpp
)

set(APP_CORE_FILES
    # --- app/actions
    actions/include/app/actions/saction.h
    actions/include/app/actions/sactionhistory.h
    actions/include/app/actions/sactionqueue.h
    actions/include/app/actions/sactionregistry.h
    actions/include/app/actions/sactionundocommand.h
    actions/include/app/actions/scompositeaction.h
    actions/include/app/actions/scycleaction.h
    actions/include/app/actions/sgridaction.h
    actions/include/app/actions/smetronomeaction.h
    actions/include/app/actions/srenderaction.h
    actions/include/app/actions/ssetpropertyaction.h
    actions/include/app/actions/ssnaptogridaction.h
    actions/include/app/actions/stoggleplaybackaction.h
    actions/include/app/actions/stogglesettingaction.h
    actions/src/sactionhistory.cpp
    actions/src/sactionqueue.cpp
    actions/src/sactionregistry.cpp
    actions/src/sactionundocommand.cpp
    actions/src/scompositeaction.cpp
    actions/src/scycleaction.cpp
    actions/src/sgridaction.cpp
    actions/src/smetronomeaction.cpp
    actions/src/srenderaction.cpp
    actions/src/ssetprojectchannelsaction.cpp
    actions/src/ssetpropertyaction.cpp
    actions/src/ssnaptogridaction.cpp
    actions/src/stoggleplaybackaction.cpp
    # --- app/media (proposal 38 gate 1). No widget, ever: the source ABI,
    # the local provider and the id -> source registry. See
    # main/media/CONTRACT.md -- the compiler does NOT enforce the no-widget
    # rule (app_model links Qt::Widgets PUBLIC), a grep in media_source_test
    # does.
    media/include/app/media/sdelayedlocalsource.h
    media/include/app/media/slocalmediasource.h
    media/include/app/media/smediacache.h
    media/include/app/media/smediacredentials.h
    media/include/app/media/smediadrop.h
    media/include/app/media/smediaref.h
    media/include/app/media/smediaregistry.h
    media/include/app/media/smediasource.h
    media/include/app/media/smediatypes.h
    media/include/app/media/swebdavclient.h
    media/include/app/media/swebdavmediasource.h
    media/src/sdelayedlocalsource.cpp
    media/src/slocalmediasource.cpp
    media/src/smediacache.cpp
    media/src/smediadrop.cpp
    media/src/smedialocalwalker.h
    media/src/smedialocalwalker.cpp
    media/src/smediaref.cpp
    media/src/smediaregistry.cpp
    media/src/smediasource.cpp
    media/src/smediatypes.cpp
    # proposal 38 gate 4: the Nextcloud/WebDAV connector. UNIT-TEST ONLY --
    # no account, no UI, no qxa case reaches this yet (gate 5c). PROPFIND/GET
    # transport (swebdavclient) + the SMediaSource adapter (swebdavmediasource).
    media/src/swebdavclient.cpp
    media/src/swebdavmediasource.cpp
    # --- app/persistence
    persistence/include/app/persistence/sloadprojectaction.h
    persistence/include/app/persistence/sprojectloader.h
    persistence/include/app/persistence/ssaveprojectaction.h
    persistence/src/sloadprojectaction.cpp
    persistence/src/sprojectloader.cpp
    persistence/src/ssaveprojectaction.cpp
    # --- app/selection
    selection/include/app/selection/saddtoselectionaction.h
    selection/include/app/selection/sclearselectionaction.h
    selection/include/app/selection/sremovefromselectionaction.h
    selection/include/app/selection/sselectionmanager.h
    selection/include/app/selection/ssetselectionaction.h
    selection/include/app/selection/stoggleselectionaction.h
    selection/src/saddtoselectionaction.cpp
    selection/src/sclearselectionaction.cpp
    selection/src/sremovefromselectionaction.cpp
    selection/src/sselectionmanager.cpp
    selection/src/ssetselectionaction.cpp
    selection/src/stoggleselectionaction.cpp
)

set(APP_OBJECTS_FILES
    # --- app/objects/cut
    objects/cut/include/app/objects/cut/saddsampleaction.h
    objects/cut/include/app/objects/cut/scut.h
    objects/cut/include/app/objects/cut/scutrndrinline.h
    objects/cut/include/app/objects/cut/sduplicateclipaction.h
    objects/cut/include/app/objects/cut/sremoveclipaction.h
    objects/cut/include/app/objects/cut/sremovesampleaction.h
    objects/cut/include/app/objects/cut/srestorecontainerclipaction.h
    objects/cut/include/app/objects/cut/sresizeclipaction.h
    objects/cut/include/app/objects/cut/saddtakeaction.h
    objects/cut/include/app/objects/cut/sremovetakeaction.h
    objects/cut/include/app/objects/cut/sselecttakeaction.h
    objects/cut/include/app/objects/cut/ssetclipnameaction.h
    objects/cut/include/app/objects/cut/ssetclippanaction.h
    objects/cut/include/app/objects/cut/ssetclipvolumeaction.h
    objects/cut/include/app/objects/cut/ssetpitchaction.h
    objects/cut/include/app/objects/cut/ssetformantpreserveaction.h
    objects/cut/include/app/objects/cut/ssetformantshiftaction.h
    objects/cut/include/app/objects/cut/splaceclipaction.h
    objects/cut/include/app/objects/cut/splacerecordingaction.h
    objects/cut/include/app/objects/cut/ssplitclipaction.h
    objects/cut/include/app/objects/cut/stakehelpers.h
    objects/cut/include/app/objects/cut/stakestack.h
    objects/cut/include/app/objects/cut/sunsplitclipaction.h
    objects/cut/include/app/objects/cut/swarpmarkeractions.h
    objects/cut/src/saddsampleaction.cpp
    objects/cut/src/saddtakeaction.cpp
    objects/cut/src/scut.cpp
    objects/cut/src/scutrndrinline.cpp
    objects/cut/src/sduplicateclipaction.cpp
    objects/cut/src/sremoveclipaction.cpp
    objects/cut/src/sremovesampleaction.cpp
    objects/cut/src/srestorecontainerclipaction.cpp
    objects/cut/src/sremovetakeaction.cpp
    objects/cut/src/sresizeclipaction.cpp
    objects/cut/src/sselecttakeaction.cpp
    objects/cut/src/ssetclipnameaction.cpp
    objects/cut/src/ssetclippanaction.cpp
    objects/cut/src/ssetclipvolumeaction.cpp
    objects/cut/src/ssetpitchaction.cpp
    objects/cut/src/ssetformantpreserveaction.cpp
    objects/cut/src/ssetformantshiftaction.cpp
    objects/cut/src/splaceclipaction.cpp
    objects/cut/src/splacerecordingaction.cpp
    objects/cut/src/ssplitclipaction.cpp
    objects/cut/src/stakehelpers.cpp
    objects/cut/src/stakestack.cpp
    objects/cut/src/sunsplitclipaction.cpp
    objects/cut/src/swarpmarkeractions.cpp
    # --- app/objects/wave
    objects/wave/include/app/objects/wave/splainwave.h
    objects/wave/include/app/objects/wave/splainwaverndrinline.h
    objects/wave/include/app/objects/wave/srecordingcontent.h
    objects/wave/include/app/objects/wave/srecordingrndrinline.h
    objects/wave/include/app/objects/wave/swaveformdraw.h
    objects/wave/src/splainwave.cpp
    objects/wave/src/splainwaverndrinline.cpp
    objects/wave/src/srecordingcontent.cpp
    objects/wave/src/srecordingrndrinline.cpp
    objects/wave/src/swaveformdraw.cpp
    # --- app/objects/midi
    objects/midi/include/app/objects/midi/smidiactionsupport.h
    objects/midi/include/app/objects/midi/smidiclipactions.h
    objects/midi/include/app/objects/midi/smidirecordactions.h
    objects/midi/include/app/objects/midi/smidicut.h
    objects/midi/include/app/objects/midi/smidieventactions.h
    objects/midi/include/app/objects/midi/smidifileactions.h
    objects/midi/include/app/objects/midi/smidirndrinline.h
    objects/midi/include/app/objects/midi/smidisequence.h
    objects/midi/src/smidiactionsupport.cpp
    objects/midi/src/smidiclipactions.cpp
    objects/midi/src/smidirecordactions.cpp
    objects/midi/src/smidicut.cpp
    objects/midi/src/smidieventactions.cpp
    objects/midi/src/smidifileactions.cpp
    objects/midi/src/smidirndrinline.cpp
    objects/midi/src/smidisequence.cpp
    # --- app/objects/track
    objects/track/include/app/objects/track/sinsertpluginaction.h
    objects/track/include/app/objects/track/sinstrumenttracks.h
    objects/track/include/app/objects/track/sautomationactions.h
    objects/track/include/app/objects/track/seteditgroupaction.h
    objects/track/include/app/objects/track/smoveclipaction.h
    objects/track/include/app/objects/track/spluginactionsupport.h
    objects/track/include/app/objects/track/sfeelflowbounce.h
    objects/track/include/app/objects/track/slearnfeelflowaction.h
    objects/track/include/app/objects/track/ssetfeelflowmodeaction.h
    objects/track/include/app/objects/track/spluginchain.h
    objects/track/include/app/objects/track/spluginslot.h
    objects/track/include/app/objects/track/sremovepluginaction.h
    objects/track/include/app/objects/track/sreorderpluginaction.h
    objects/track/include/app/objects/track/ssetpluginbypassaction.h
    objects/track/include/app/objects/track/ssetpluginparamaction.h
    objects/track/include/app/objects/track/sliveinputactions.h
    objects/track/include/app/objects/track/ssettrackmidiroutingaction.h
    objects/track/include/app/objects/track/ssettrackmuteaction.h
    objects/track/include/app/objects/track/ssettracknameaction.h
    objects/track/include/app/objects/track/ssettracksoloaction.h
    objects/track/include/app/objects/track/ssettrackvolumeaction.h
    objects/track/include/app/objects/track/strack.h
    objects/track/include/app/objects/track/strackcolormodifier.h
    objects/track/include/app/objects/track/strackpath.h
    objects/track/include/app/objects/track/strackrenderer.h
    objects/track/include/app/objects/track/strackrndrinline.h
    objects/track/src/sautomationactions.cpp
    objects/track/src/seteditgroupaction.cpp
    objects/track/src/sinsertpluginaction.cpp
    objects/track/src/sinstrumenttracks.cpp
    objects/track/src/smoveclipaction.cpp
    objects/track/src/sfeelflowbounce.cpp
    objects/track/src/slearnfeelflowaction.cpp
    objects/track/src/ssetfeelflowmodeaction.cpp
    objects/track/src/spluginchain.cpp
    objects/track/src/spluginslot.cpp
    objects/track/src/sremovepluginaction.cpp
    objects/track/src/sreorderpluginaction.cpp
    objects/track/src/ssetpluginbypassaction.cpp
    objects/track/src/ssetpluginparamaction.cpp
    objects/track/include/app/objects/track/ssettrackmidioutputaction.h
    objects/track/src/sliveinputactions.cpp
    objects/track/src/ssettrackmidioutputaction.cpp
    objects/track/src/ssettrackmidiroutingaction.cpp
    objects/track/src/ssettrackmuteaction.cpp
    objects/track/src/ssettracknameaction.cpp
    objects/track/src/ssettracksoloaction.cpp
    objects/track/src/ssettrackvolumeaction.cpp
    objects/track/src/strack.cpp
    objects/track/src/strackcolormodifier.cpp
    objects/track/src/strackrndrinline.cpp
    # --- app/objects/mixer
    objects/mixer/include/app/objects/mixer/saddtrackaction.h
    objects/mixer/include/app/objects/mixer/sdissolvearrangementaction.h
    objects/mixer/include/app/objects/mixer/sextractarrangementaction.h
    objects/mixer/include/app/objects/mixer/screatearrangementaction.h
    objects/mixer/include/app/objects/mixer/screateassetaction.h
    objects/mixer/include/app/objects/mixer/smovetrackaction.h
    objects/mixer/include/app/objects/mixer/splaceassetaction.h
    objects/mixer/include/app/objects/mixer/sremovearrangementaction.h
    objects/mixer/include/app/objects/mixer/sremoveassetaction.h
    objects/mixer/include/app/objects/mixer/sremoveassetplacementaction.h
    objects/mixer/include/app/objects/mixer/sremovetrackaction.h
    objects/mixer/include/app/objects/mixer/sreparenttrackaction.h
    objects/mixer/include/app/objects/mixer/srestoretrackaction.h
    objects/mixer/include/app/objects/mixer/sstdmixer.h
    objects/mixer/src/saddtrackaction.cpp
    objects/mixer/src/sdissolvearrangementaction.cpp
    objects/mixer/src/sextractarrangementaction.cpp
    objects/mixer/src/screatearrangementaction.cpp
    objects/mixer/src/screateassetaction.cpp
    objects/mixer/src/smovetrackaction.cpp
    objects/mixer/src/splaceassetaction.cpp
    objects/mixer/src/sremovearrangementaction.cpp
    objects/mixer/src/sremoveassetaction.cpp
    objects/mixer/src/sremoveassetplacementaction.cpp
    objects/mixer/src/sremovetrackaction.cpp
    objects/mixer/src/sreparenttrackaction.cpp
    objects/mixer/src/srestoretrackaction.cpp
    objects/mixer/src/sstdmixer.cpp
)

set(APP_UI_FILES
    # --- app/timeline
    timeline/include/app/timeline/sclippropertiespanel.h
    timeline/include/app/timeline/sgridtoolbar.h
    timeline/include/app/timeline/sobjecttreeview.h
    timeline/include/app/timeline/ssmvmixercontrol.h
    timeline/include/app/timeline/sstdmixerview.h
    timeline/include/app/timeline/strackdetailpanel.h
    timeline/include/app/timeline/sfeelflowpanel.h
    timeline/include/app/timeline/sfadercurve.h
    timeline/include/app/timeline/slevelmeter.h
    timeline/include/app/timeline/strackheaderresizer.h
    timeline/include/app/timeline/szoomscrollbar.h
    timeline/src/sautomationlane.h
    timeline/src/sautomationlane.cpp
    timeline/src/sfeelflowpanel.cpp
    timeline/src/sclippropertiespanel.cpp
    timeline/src/sgridtoolbar.cpp
    timeline/src/sobjecttreeview.cpp
    timeline/src/ssmvmixercontrol.cpp
    timeline/src/sstdmixerview.cpp
    timeline/src/strackdetailpanel.cpp
    timeline/src/slevelmeter.cpp
    timeline/src/strackheaderresizer.cpp
    timeline/src/szoomscrollbar.cpp
    # --- app/eventui (proposal 37 P4)
    eventui/include/app/eventui/seventeditordock.h
    eventui/include/app/eventui/seventeditorview.h
    eventui/include/app/eventui/seventtimeaxis.h
    eventui/include/app/eventui/seventtimeruler.h
    eventui/include/app/eventui/spianorollview.h
    eventui/include/app/eventui/svirtualkeyboarddock.h
    eventui/src/seventeditordock.cpp
    eventui/src/seventeditorview.cpp
    eventui/src/seventtimeaxis.cpp
    eventui/src/seventtimeruler.cpp
    eventui/src/spianorollview.cpp
    eventui/src/svirtualkeyboarddock.cpp
    # --- app/mediabrowser (proposal 38 gate 2). The media browser DOCK, and
    # nothing else: the source ABI, the providers and (later) the cache live in
    # app/media, one layer down, where no widget may appear. Rank of
    # pluginui/eventui -- a UI slice that is hosted by the shell.
    mediabrowser/include/app/mediabrowser/smediabrowserpanel.h
    mediabrowser/src/smediabrowserpanel.cpp
    # --- app/pluginui
    pluginui/include/app/pluginui/spluginbrowserdialog.h
    pluginui/include/app/pluginui/splugineffectstrip.h
    pluginui/include/app/pluginui/spluginnativeeditor.h
    pluginui/include/app/pluginui/spluginparamereditor.h
    pluginui/src/spluginbrowserdialog.cpp
    pluginui/src/splugineffectstrip.cpp
    pluginui/src/spluginnativeeditor.cpp
    pluginui/src/spluginparamereditor.cpp
    # --- app/servicesui
    servicesui/include/app/servicesui/scleanupdialog.h
    servicesui/include/app/servicesui/slogmodel.h
    servicesui/include/app/servicesui/slogview.h
    servicesui/include/app/servicesui/soptions.h
    servicesui/include/app/servicesui/soptionsdialog.h
    servicesui/include/app/servicesui/srecordingprogress.h
    servicesui/include/app/servicesui/srenderdialog.h
    servicesui/include/app/servicesui/srenderprogress.h
    servicesui/include/app/servicesui/stransportoptionactions.h
    servicesui/src/scleanupdialog.cpp
    servicesui/src/slogmodel.cpp
    servicesui/src/slogview.cpp
    servicesui/src/soptions.cpp
    servicesui/src/soptionsdialog.cpp
    servicesui/src/srecordingprogress.cpp
    servicesui/src/srenderdialog.cpp
    servicesui/src/srenderprogress.cpp
    servicesui/src/stransportoptionactions.cpp
    # --- app/shell
    shell/include/app/shell/sapplication.h
    shell/include/app/shell/sautomationrecorder.h
    shell/include/app/shell/saudiorecorder.h
    shell/include/app/shell/srecordplacement.h
    shell/include/app/shell/sliveinputsource.h
    shell/include/app/shell/slivemonitor.h
    shell/include/app/shell/sliveplanbuilder.h
    shell/include/app/shell/smediaaccountmanager.h
    shell/include/app/shell/smidiinputhub.h
    shell/include/app/shell/smidioutpump.h
    shell/include/app/shell/smidirecorder.h
    shell/include/app/shell/splayheadclock.h
    shell/include/app/shell/smainwindow.h
    shell/include/app/shell/ssecretstore.h
    shell/include/app/shell/ssettings.h
    shell/src/sautomationrecorder.cpp
    shell/src/sapplication.cpp
    shell/src/saudiorecorder.cpp
    shell/src/sliveinputsource.cpp
    shell/src/slivemonitor.cpp
    shell/src/sliveplanbuilder.cpp
    shell/src/smediaaccountmanager.cpp
    shell/src/smidiinputhub.cpp
    shell/src/smidioutpump.cpp
    shell/src/smidirecorder.cpp
    shell/src/smainwindow.cpp
    shell/src/ssecretstore.cpp
    shell/src/ssecretstorebackend.h
    shell/src/ssettings.cpp
    # --- app/testkit
    testkit/include/app/testkit/sactionrunner.h
    testkit/include/app/testkit/sactionscript.h
    testkit/include/app/testkit/sassertaudioenergyaction.h
    testkit/include/app/testkit/sassertautomationvalueaction.h
    testkit/include/app/testkit/sautomationuitestactions.h
    testkit/include/app/testkit/sassertaudiofrequencyaction.h
    testkit/include/app/testkit/sassertaudiolengthaction.h
    testkit/include/app/testkit/sassertaudiopeakaction.h
    testkit/include/app/testkit/sassertchannelsdifferaction.h
    testkit/include/app/testkit/sassertclipchannelsaction.h
    testkit/include/app/testkit/sasserttrackchannelsaction.h
    testkit/include/app/testkit/sassertfilecontainsaction.h
    testkit/include/app/testkit/sassertclipwindowaction.h
    testkit/include/app/testkit/sassertclipmixaction.h
    testkit/include/app/testkit/sassertenvelopeaction.h
    testkit/include/app/testkit/sassertfileidenticalaction.h
    testkit/include/app/testkit/sassertlogaction.h
    testkit/include/app/testkit/slivetestactions.h
    testkit/include/app/testkit/srecordtestactions.h
    testkit/include/app/testkit/smidirecordtestactions.h
    testkit/include/app/testkit/sassertsourcepositionaction.h
    testkit/include/app/testkit/sdragclipedgeaction.h
    testkit/include/app/testkit/seventuitestactions.h
    testkit/include/app/testkit/sdumpplaybackcaptureaction.h
    testkit/include/app/testkit/sgrouptrackaction.h
    testkit/include/app/testkit/slanelayouttestactions.h
    testkit/include/app/testkit/slogstressaction.h
    testkit/include/app/testkit/smediatestactions.h
    testkit/include/app/testkit/smetertestactions.h
    testkit/include/app/testkit/smiditestactions.h
    testkit/include/app/testkit/spluginuitestactions.h
    testkit/include/app/testkit/sreportpagememoryaction.h
    testkit/include/app/testkit/sscreenshotaction.h
    testkit/include/app/testkit/sclicklaneaction.h
    testkit/include/app/testkit/ssidecartestactions.h
    testkit/include/app/testkit/sslipclipaction.h
    testkit/include/app/testkit/stestfilepath.h
    testkit/include/app/testkit/strackselectionactions.h
    testkit/include/app/testkit/stransporttestactions.h
    testkit/src/sactionrunner.cpp
    testkit/src/sactionscript.cpp
    testkit/src/sassertaudioenergyaction.cpp
    testkit/src/sassertautomationvalueaction.cpp
    testkit/src/sautomationuitestactions.cpp
    testkit/src/sassertaudiofrequencyaction.cpp
    testkit/src/sassertaudiolengthaction.cpp
    testkit/src/sassertaudiopeakaction.cpp
    testkit/src/sassertchannelsdifferaction.cpp
    testkit/src/sassertclipchannelsaction.cpp
    testkit/src/sasserttrackchannelsaction.cpp
    testkit/src/sassertfilecontainsaction.cpp
    testkit/src/sassertclipwindowaction.cpp
    testkit/src/sassertclipmixaction.cpp
    testkit/src/sassertenvelopeaction.cpp
    testkit/src/sassertfileidenticalaction.cpp
    testkit/src/sassertlogaction.cpp
    testkit/src/slivetestactions.cpp
    testkit/src/srecordtestactions.cpp
    testkit/src/smidirecordtestactions.cpp
    testkit/src/sassertsourcepositionaction.cpp
    testkit/src/sclicklaneaction.cpp
    testkit/src/sdragclipedgeaction.cpp
    testkit/src/seventuitestactions.cpp
    testkit/src/sdumpplaybackcaptureaction.cpp
    testkit/src/sgrouptrackaction.cpp
    testkit/src/slanelayouttestactions.cpp
    testkit/src/slogstressaction.cpp
    testkit/src/smediatestactions.cpp
    testkit/src/smetertestactions.cpp
    testkit/include/app/testkit/smidiouttestactions.h
    testkit/src/smidiouttestactions.cpp
    testkit/src/smidiintestactions.cpp
    testkit/src/smiditestactions.cpp
    testkit/src/spluginuitestactions.cpp
    testkit/src/sreportpagememoryaction.cpp
    testkit/src/sscreenshotaction.cpp
    testkit/src/ssidecartestactions.cpp
    testkit/src/sslipclipaction.cpp
    testkit/src/stestfilepath.cpp
    testkit/src/sarrangementtestactions.cpp
    testkit/src/strackselectionactions.cpp
    testkit/src/stransporttestactions.cpp
    # proposal 38 gate 4: the WebDAV stub server. Deliberately NOT under
    # include/app/testkit/ -- it is not part of testkit's public action-verb
    # API, only a fixture two things link directly: this gate's own
    # webdav_source_test (main/media/tests/, via an explicit extra include
    # dir -- see the webdav_source_test target below) and gate 5c's
    # `media-webdav-stub` verb, which will live in smaragd.exe.
    testkit/src/swebdavstub.h
    testkit/src/swebdavstub.cpp
)

# Layer 1 — the document model (+ the Phase 6 seams: SAppContext,
# sdetaileditors, sobjectpath). Engine access: exactly APP_ENG(model).
add_library(app_model OBJECT ${APP_MODEL_FILES})
target_include_directories(app_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/model/include)
target_link_libraries(app_model
    PUBLIC
        tw_core tw_graph tw_pages tw_schedule tw_sources tw_events
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Xml
)

# Layer 2 — command framework, persistence, selection.
add_library(app_core OBJECT ${APP_CORE_FILES})
target_include_directories(app_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/include
    ${CMAKE_CURRENT_SOURCE_DIR}/media/include
    ${CMAKE_CURRENT_SOURCE_DIR}/persistence/include
    ${CMAKE_CURRENT_SOURCE_DIR}/selection/include
)
# Qt::Network and Qt::Concurrent arrive with app/media (proposal 38). Neither
# is used by gate 1 -- Network is gate 4's WebDAV client -- but they are linked
# on the layer rather than the module because LAYERS, not modules, are the
# CMake targets (§B.1). They propagate upward; that is the one accepted
# impurity.
target_link_libraries(app_core PUBLIC
    app_model tw_render
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Concurrent
)

# Layer 3 — the object slices (cut, wave, track, mixer).
add_library(app_objects OBJECT ${APP_OBJECTS_FILES})
target_include_directories(app_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/cut/include
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/midi/include
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/wave/include
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/track/include
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/mixer/include
)
target_link_libraries(app_objects PUBLIC app_core tw_mix tw_plugins tw_sidecar)

# Layer 4 — the UI and the shell (the only layer that may see everything).
add_library(app_ui OBJECT ${APP_UI_FILES})
target_include_directories(app_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline/include
    ${CMAKE_CURRENT_SOURCE_DIR}/eventui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/mediabrowser/include
    ${CMAKE_CURRENT_SOURCE_DIR}/pluginui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/servicesui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shell/include
    ${CMAKE_CURRENT_SOURCE_DIR}/testkit/include
)
# XPM icon includes ("pix/zoomin.xpm") resolve from the smaragd/ source root.
target_include_directories(app_ui PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(app_ui PUBLIC app_objects tw303a)

# --- SSecretStore backends (proposal 38 GATE 5a, main/shell/CONTRACT.md) -----
# `dpapi` and `keychain` follow the platform unconditionally -- crypt32 ships
# with MinGW and Security.framework ships in the macOS SDK, so neither is a
# new dependency (mirrors ENABLE_WASAPI/ENABLE_COREAUDIO's "always available on
# their platform" shape in the top-level CMakeLists.txt). `libsecret` is
# genuinely optional: TW_HAVE_LIBSECRET defaults to whatever pkg-config found
# at configure time, exactly the TW_HAVE_CLAP/TW_HAVE_VST3 discipline in
# tw303a/CMakeLists.txt -- when it is OFF, ssecretstore_linux.cpp is not even
# added to the build, and resolveBackend()'s Linux default falls through to
# "none".
if(SMARAGD_WINDOWS)
    target_sources(app_ui PRIVATE
        shell/src/ssecretstore_win.cpp
    )
    target_compile_definitions(app_ui PRIVATE TW_HAVE_DPAPI=1)
    target_link_libraries(app_ui PRIVATE crypt32)
    message(STATUS "app_ui: SSecretStore DPAPI backend enabled")
endif()

if(SMARAGD_MACOS)
    find_library(SMARAGD_SECURITY_FRAMEWORK Security)
    target_sources(app_ui PRIVATE
        shell/src/ssecretstore_mac.mm
    )
    target_compile_definitions(app_ui PRIVATE TW_HAVE_KEYCHAIN=1)
    target_link_libraries(app_ui PRIVATE ${SMARAGD_SECURITY_FRAMEWORK})
    message(STATUS "app_ui: SSecretStore Keychain backend enabled (UNTESTED on the Windows box this gate was built on)")
endif()

set(_smaragd_have_libsecret FALSE)
if(SMARAGD_LINUX)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSECRET libsecret-1)
        if(LIBSECRET_FOUND)
            set(_smaragd_have_libsecret TRUE)
        endif()
    endif()
endif()
option(TW_HAVE_LIBSECRET
    "Build the libsecret credential backend for SSecretStore (Linux)"
    ${_smaragd_have_libsecret})
if(SMARAGD_LINUX AND TW_HAVE_LIBSECRET)
    target_sources(app_ui PRIVATE
        shell/src/ssecretstore_linux.cpp
    )
    target_compile_definitions(app_ui PRIVATE TW_HAVE_LIBSECRET=1)
    target_link_libraries(app_ui PRIVATE PkgConfig::LIBSECRET)
    message(STATUS "app_ui: SSecretStore libsecret backend enabled (UNTESTED on the Windows box this gate was built on)")
elseif(SMARAGD_LINUX)
    message(STATUS "app_ui: libsecret not found; SSecretStore falls back to the 'none' backend on Linux "
                    "(install libsecret-1-dev and re-configure to enable it)")
endif()

# Optionally embed a bundled UI font. Drop a TTF at main/resources/FreeSans.ttf
# and re-run CMake: it is compiled into the binary (AUTORCC) and used as the
# default, antialiased UI font (see main.cpp). If absent, the platform default
# font is used and the build is unaffected. (GNU FreeFont / FreeSans is GPL with
# a font exception, so bundling it in the application is fine.)
set(SMARAGD_MAIN_SOURCES shell/src/main.cpp)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources/FreeSans.ttf)
    list(APPEND SMARAGD_MAIN_SOURCES resources/fonts.qrc)
    message(STATUS "Smaragd: embedding bundled font resources/FreeSans.ttf")
else()
    message(STATUS "Smaragd: resources/FreeSans.ttf not found; using the platform default font")
endif()

# SMARAGD_WIN_CONSOLE: when ON (the default), the Windows build uses the
# CONSOLE subsystem so stderr/stdout — including the twsyslog.h shim's
# vfprintf calls and Qt's qDebug() — appear in the launching console.
# Flip to OFF for a release build that should not pop a console window.
option(SMARAGD_WIN_CONSOLE
    "Build the Windows binary against the CONSOLE subsystem (shows stderr)" ON)

# SMARAGD_LOG_CONSOLE_DEFAULT (proposal 24): whether TwLog tees to stderr when
# the user has expressed no preference. ORTHOGONAL to SMARAGD_WIN_CONSOLE, which
# only decides whether Windows gives the process a console to write to. Defaults
# to ON for a Debug build and OFF otherwise — "console by default in debug, by
# option in release". The persisted SOpt::LogConsole, then SMARAGD_LOG_CONSOLE in
# the environment, then --log-console/--no-log-console each override it.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
    set(SMARAGD_LOG_CONSOLE_DEFAULT_INIT ON)
else()
    set(SMARAGD_LOG_CONSOLE_DEFAULT_INIT OFF)
endif()
option(SMARAGD_LOG_CONSOLE_DEFAULT
    "Tee the log to stderr unless overridden (default: ON for Debug builds)"
    ${SMARAGD_LOG_CONSOLE_DEFAULT_INIT})
message(STATUS "Smaragd: log console default is ${SMARAGD_LOG_CONSOLE_DEFAULT}")

# PUBLIC so the smaragd executable (main.cpp) inherits it along with app_ui.
target_compile_definitions(app_ui PUBLIC
    SMARAGD_LOG_CONSOLE_DEFAULT=$<BOOL:${SMARAGD_LOG_CONSOLE_DEFAULT}>)

if(SMARAGD_WINDOWS AND NOT SMARAGD_WIN_CONSOLE)
    add_executable(smaragd WIN32 ${SMARAGD_MAIN_SOURCES})
elseif(SMARAGD_MACOS)
    add_executable(smaragd MACOSX_BUNDLE ${SMARAGD_MAIN_SOURCES})

    # The out-of-process plugin probe has to live INSIDE the bundle: the app
    # locates it as applicationDirPath()/smaragd_pluginprobe, which on macOS is
    # Contents/MacOS. Added BEFORE the codesign command below on purpose —
    # POST_BUILD steps run in the order they are declared, and dropping a file
    # into the bundle after --deep --sign would leave the sealed resources stale.
    if(TARGET smaragd_pluginprobe)
        add_custom_command(TARGET smaragd POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:smaragd_pluginprobe>"
                    "$<TARGET_BUNDLE_DIR:smaragd>/Contents/MacOS/"
            COMMENT "Copying smaragd_pluginprobe into the app bundle"
        )
    endif()

    # The twtestclap fixture is resolved by qxa plugin cases through a RELATIVE
    # path against QCoreApplication::applicationDirPath() (SPluginSlot::
    # resolveModulePath). On macOS that directory is Contents/MacOS INSIDE the
    # bundle, whereas tw303a drops the fixture next to the .app (build/bin) — so
    # it must be copied in here as well, or every plugin qxa case loads a
    # transparent Missing placeholder and its audio assertions fail. Same
    # before-codesign ordering rule as the probe above. Flat .clap dylib: the
    # macOS loader now dlopens a plain-file .clap directly (twclapmodule.cc).
    if(TARGET twtestclap)
        add_custom_command(TARGET smaragd POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:twtestclap>"
                    "$<TARGET_BUNDLE_DIR:smaragd>/Contents/MacOS/twtestclap.clap"
            COMMENT "Copying the twtestclap fixture into the app bundle (qxa cases)"
        )
    endif()

    # Code-sign app with microphone entitlements after building
    add_custom_command(TARGET smaragd POST_BUILD
        COMMAND codesign --force --deep --sign -
                --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/smaragd.entitlements"
                "$<TARGET_BUNDLE_DIR:smaragd>"
        COMMENT "Code-signing smaragd with microphone entitlements"
    )
else()
    add_executable(smaragd ${SMARAGD_MAIN_SOURCES})
endif()

if(SMARAGD_MACOS)
    set_target_properties(smaragd PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.tweggen.smaragd"
        MACOSX_BUNDLE_BUNDLE_NAME "Smaragd"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )
endif()

target_link_libraries(smaragd
    PRIVATE
        app_ui app_objects app_core app_model
)

# Not a link dependency — a SEPARATE executable the app spawns at scan time
# (proposal 08 M2). Declared so building just the `smaragd` target still produces
# it; without it the app silently falls back to in-process probing, i.e. loses
# its crash isolation, which is the kind of regression nobody notices.
if(TARGET smaragd_pluginprobe)
    add_dependencies(smaragd smaragd_pluginprobe)
endif()

# The macOS POST_BUILD above copies the twtestclap fixture into the bundle before
# codesigning; make it exist first. Only relevant on macOS (the copy is gated by
# SMARAGD_MACOS), harmless elsewhere.
if(SMARAGD_MACOS AND TARGET twtestclap)
    add_dependencies(smaragd twtestclap)
endif()

# Auto-deploy Qt runtime DLLs / plugins next to smaragd.exe on Windows
# (and the .app bundle on macOS) so the binary launches without a manual
# windeployqt step. Opt out with -DAUTO_DEPLOY_QT=OFF.
option(AUTO_DEPLOY_QT
    "Run windeployqt / macdeployqt after building smaragd" ON)

if(AUTO_DEPLOY_QT AND (SMARAGD_WINDOWS OR SMARAGD_MACOS))
    # windeployqt / macdeployqt live next to qmake in the Qt bin dir.
    get_target_property(_qt_qmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_qmake}" DIRECTORY)

    if(SMARAGD_WINDOWS)
        find_program(DEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")
        set(DEPLOYQT_ARGS --no-translations --no-system-d3d-compiler --no-opengl-sw)
    else()
        find_program(DEPLOYQT_EXECUTABLE macdeployqt HINTS "${_qt_bin_dir}")
        set(DEPLOYQT_ARGS "")
    endif()

    if(DEPLOYQT_EXECUTABLE)
        if(SMARAGD_MACOS)
            # macdeployqt expects the .app bundle directory, not the executable
            set(_DEPLOY_TARGET "$<TARGET_BUNDLE_DIR:smaragd>")
        else()
            # windeployqt expects the executable path
            set(_DEPLOY_TARGET "$<TARGET_FILE:smaragd>")
        endif()

        add_custom_command(TARGET smaragd POST_BUILD
            COMMAND "${DEPLOYQT_EXECUTABLE}" ${DEPLOYQT_ARGS} "${_DEPLOY_TARGET}"
            COMMENT "Deploying Qt runtime alongside $<TARGET_FILE_NAME:smaragd>"
            VERBATIM
        )

        # windeployqt deploys only the platform plugin the APP needs — qwindows.
        # But every headless gate (the ~90 qxa cases via --test-case, plus
        # action_roundtrip_test / filepathref_test / preview_container_test) runs
        # with QT_QPA_PLATFORM=offscreen, and qoffscreen.dll is NOT deployed.
        # Its absence does not fail fast: Qt blocks in a "this application failed
        # to start" platform-plugin dialog that nobody can see in a headless run,
        # so each affected test burns its ENTIRE timeout (300 s) at ~0 s of CPU
        # instead of passing in ~0.05 s. That has cost real gate runs ten
        # minutes of pure timeout and reads as a suite problem rather than a
        # missing file, so deploy it here instead of leaving it to be
        # rediscovered.
        if(TARGET Qt${QT_VERSION_MAJOR}::QOffscreenIntegrationPlugin)
            add_custom_command(TARGET smaragd POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:smaragd>/platforms"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::QOffscreenIntegrationPlugin>"
                        "$<TARGET_FILE_DIR:smaragd>/platforms/"
                COMMENT "Deploying the offscreen platform plugin (headless tests)"
                VERBATIM
            )
        else()
            message(WARNING
                "Qt${QT_VERSION_MAJOR}::QOffscreenIntegrationPlugin was not found, so "
                "qoffscreen.dll cannot be deployed. Headless tests that set "
                "QT_QPA_PLATFORM=offscreen will HANG until their timeout rather "
                "than fail fast. Copy it next to the binary by hand: "
                "<QtPrefix>/plugins/platforms/qoffscreen.* -> "
                "build/bin/platforms/.")
        endif()
    else()
        message(WARNING
            "AUTO_DEPLOY_QT is ON but the deploy tool was not found in "
            "${_qt_bin_dir}. The binary will need its Qt DLLs supplied "
            "manually (PATH or copy).")
    endif()
endif()

# vcpkg render deps (libsndfile/libvorbis/libogg and their codec DLLs) are
# dynamic on the x64-mingw-dynamic triplet; copy them next to the exe so the
# binary stays self-contained (windeployqt handles only the Qt DLLs). Without
# this the exe silently fails to start after a clean build.
if(SMARAGD_WINDOWS AND DEFINED VCPKG_INSTALLED_DIR)
    file(GLOB _VCPKG_RUNTIME_DLLS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/*.dll")
    # Rubber Band was removed 2026-07-26 (GPL relicensing) — its DLL may
    # still sit in the shared vcpkg install for other projects, but it must
    # not be deployed next to a binary that no longer links it.
    list(FILTER _VCPKG_RUNTIME_DLLS EXCLUDE REGEX "rubberband")
    if(_VCPKG_RUNTIME_DLLS)
        add_custom_command(TARGET smaragd POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_VCPKG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:smaragd>"
            COMMENT "Copying vcpkg runtime DLLs next to smaragd.exe"
            VERBATIM
        )
    endif()
endif()

# Action round-trip test (Phase 0 of action scripts): verify all actions
# can serialize and deserialize without loss. Links the app object library —
# no more recompiling every app source into the test binary.
add_executable(action_roundtrip_test
    testkit/src/action_roundtrip_test.cpp
)

target_link_libraries(action_roundtrip_test
    PRIVATE
        app_ui app_objects app_core app_model
)

# It was BUILT but never registered, so `ctest` never ran it: three milestones
# of new action verbs went in without the round-trip audit ever gating anything
# (proposal 08 M5). It needs a QApplication for the widget-owning verbs' static
# registrars, and offscreen so it never touches a display.
add_test(NAME action_roundtrip_test COMMAND action_roundtrip_test)
set_tests_properties(action_roundtrip_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# External-file path encoding (project-relative / "~" / absolute). Pure path
# arithmetic over app_model, so it needs no project, no audio and no display —
# but it links all four layers because app_model is an OBJECT library whose
# static registrars pull in the rest.
add_executable(filepathref_test testkit/src/filepathref_test.cpp)
target_link_libraries(filepathref_test
    PRIVATE
        app_ui app_objects app_core app_model
)
add_test(NAME filepathref_test COMMAND filepathref_test)
set_tests_properties(filepathref_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Media browser Size-column formatting (mediaBrowserFormatSize, task item k):
# at most 3 significant digits plus an optional k/M/G suffix. Pure string
# arithmetic over app_ui — no project, no audio and no display — but it links
# all four layers for the same reason filepathref_test does (app_model is an
# OBJECT library whose static registrars pull in the rest).
add_executable(mediabrowser_size_test testkit/src/mediabrowser_size_test.cpp)
target_link_libraries(mediabrowser_size_test
    PRIVATE
        app_ui app_objects app_core app_model
)
add_test(NAME mediabrowser_size_test COMMAND mediabrowser_size_test)
set_tests_properties(mediabrowser_size_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# SPluginNativeEditor::clampOntoAScreen (proposal 33 D2). A remembered editor
# geometry restored blind is the classic multi-monitor bug -- the window comes
# back on a screen that is no longer plugged in, off screen, unreachable, and on
# Windows not even reported. That cannot be reproduced by hand without
# unplugging a monitor, so the clamp is a pure function of (rect, screens) and
# this pins it. Needs a QGuiApplication for QScreen, hence offscreen; links all
# four layers for the reason mediabrowser_size_test does.
add_executable(plugin_editor_geometry_test
    pluginui/tests/plugin_editor_geometry_test.cpp)
target_link_libraries(plugin_editor_geometry_test
    PRIVATE
        app_ui app_objects app_core app_model
)
add_test(NAME plugin_editor_geometry_test COMMAND plugin_editor_geometry_test)
set_tests_properties(plugin_editor_geometry_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# SSecretStore (proposal 38 GATE 5a, §B.8): store/retrieve/delete round trips,
# byte-exact non-ASCII+NUL secrets, the ciphertext-not-plaintext + DPAPI-salts
# checks, the `none`/`memory` no-INI-write checks, and the Undecryptable
# failure modes (corrupt blob, foreign/uncompiled scheme tag) — never the
# platform cryptography itself. Constructs its OWN private-temp-file QSettings
# and never references SSettings::instance() (AC 6a), so unlike the two
# RUN_SERIAL qxa cases in gate 5b that DO touch the shared smaragd.ini, this
# binary needs no RUN_SERIAL and is safe under `ctest -j4`.
add_executable(secret_store_test shell/tests/secret_store_test.cpp)
target_link_libraries(secret_store_test
    PRIVATE
        app_ui app_objects app_core app_model
)
add_test(NAME secret_store_test COMMAND secret_store_test)
set_tests_properties(secret_store_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The CONTAINER branch of SObject::straightCalcPreviewData(): a preview built
# from a live sub-arrangement reads FROZEN PAGES (chained, page-aligned) rather
# than seeking the live component per preview window. Values, page-boundary
# indexing, state chaining and the reposition count are all asserted against a
# synthetic component — the paint path is otherwise the only consumer, and a
# screenshot cannot localise any of them. Needs a QApplication (offscreen) for
# the app object libraries' static registrars, like the two tests above.
add_executable(preview_container_test testkit/src/preview_container_test.cpp)
target_link_libraries(preview_container_test
    PRIVATE
        app_ui app_objects app_core app_model
)
add_test(NAME preview_container_test COMMAND preview_container_test)
set_tests_properties(preview_container_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The COLLECT SEAM (proposal 39 M1): SObjectRenderer::collectEnvelope() hands
# out exactly the probes the renderer DRAWS - through the link-start fold, a
# cut's slip, and loop tiling, each repetition into its own pixel span. The
# painted side is read back out of a 256-tall QImage (y = 127 - value at that
# height), so the two ends are measured against each other rather than against
# a re-run of one of them. Sibling of preview_container_test; needs a
# QApplication (offscreen) for the app object libraries' static registrars.
#
# Since M2 it also carries THE rule gate (proposal 39 M2): the drawn pixels are
# the collected probes no matter what fader the link's PARENT object holds. That
# one has to live here rather than in a .qxa, because the collect half never
# carried the volume multiply and a script reading through collectEnvelope
# cannot see it - only the pixels can.
add_executable(preview_envelope_test testkit/src/preview_envelope_test.cpp)
target_link_libraries(preview_envelope_test
    PRIVATE
        app_ui app_objects app_core app_model
)
add_test(NAME preview_envelope_test COMMAND preview_envelope_test)
set_tests_properties(preview_envelope_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# The project's CHANNEL COUNT as project data (proposal 36 M1): the default,
# the legacy warn-and-default, the round trip, and â€” the reason it is a test and
# not a code review â€” that set-project-channels and its undo make NO
# STrack::setNBusses call. M1 is inaudible on purpose, and only instrumentation
# can fail when that stops being true. Same shape as the three tests above.
add_executable(project_channels_test testkit/src/project_channels_test.cpp)
target_link_libraries(project_channels_test
    PRIVATE
        app_ui app_objects app_core app_model
)
add_test(NAME project_channels_test COMMAND project_channels_test)
set_tests_properties(project_channels_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The media SOURCE layer (proposal 38 gate 1): the async/id-tagged contract,
# supersession, cancel, main-thread delivery, and a source destroyed mid-walk.
# Three of its twelve ACs are greps over main/media's own sources (no
# std::thread, no widget include, the CONTRACT states its invariants), which is
# why the test is told where both trees are rather than guessing from argv[0].
add_executable(media_source_test media/tests/media_source_test.cpp)
target_link_libraries(media_source_test
    PRIVATE
        app_ui app_objects app_core app_model
)
target_compile_definitions(media_source_test PRIVATE
    SMEDIA_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/media"
    SMEDIA_MODULE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/media"
)
add_test(NAME media_source_test COMMAND media_source_test)
set_tests_properties(media_source_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The media CACHE and the project copy (proposal 38 gate 3): the content key's
# stability and change detection, cache hit/miss, the per-writer temp, LRU
# eviction with the project PIN (trap T17), the disabled shape, the project
# copy's name sanitisation (T19) -- and AC 8, which RE-EXECS THIS BINARY four
# times (`--cache-child`) so that "two processes fetching one key leave one
# intact file" is a real two-process claim rather than two serialised writes on
# one thread. Same shape as qxa.instrument_render_determinism_xproc.
add_executable(media_cache_test media/tests/media_cache_test.cpp)
target_link_libraries(media_cache_test
    PRIVATE
        app_ui app_objects app_core app_model
)
add_test(NAME media_cache_test COMMAND media_cache_test)
set_tests_properties(media_cache_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 300)

# The Nextcloud/WebDAV connector (proposal 38 gate 4): PROPFIND parsing, the
# BFS search's concurrency cap and truncation, GET with progress/cancel, error
# handling (401 does not retry, 404, a mid-body close), and a source destroyed
# with requests in flight -- all against the in-repo stub
# (main/testkit/src/swebdavstub.{h,cpp}), never a real server. UNIT-TEST ONLY:
# no account, no UI, no qxa case reaches this yet (gate 5c adds those behind a
# `media-webdav-stub` verb). The stub lives in main/testkit (not
# main/media/tests) because smaragd.exe itself starts one in gate 5c and
# links testkit already; this test reaches it through an explicit extra
# include dir since swebdavstub.h is not part of testkit's public
# include/app/testkit/ API.
add_executable(webdav_source_test media/tests/webdav_source_test.cpp)
target_include_directories(webdav_source_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/testkit/src
)
target_link_libraries(webdav_source_test
    PRIVATE
        app_ui app_objects app_core app_model
)
target_compile_definitions(webdav_source_test PRIVATE
    SMEDIA_MODULE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/media"
    SWEBDAV_TESTKIT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testkit/src"
)
add_test(NAME webdav_source_test COMMAND webdav_source_test)
set_tests_properties(webdav_source_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
