
#ifndef _SAPPLICATION_H
#define _SAPPLICATION_H

#include <tw303aenv.h>
#include <twcomponent.h>
#include <QApplication>
//#include <qptrlist.h>

class tw303aEnvironment;
class twSpeaker;
class twWhiteNoise;
class SObject;
class SLink;
class SProject;

typedef QList<SLink*> SSelectionList;

/**
 * This object glues all wires together.
 *
 * Besides other things, it contains various stuff that should
 * not be here and later hopefully will migrate to proper
 * objects:
 * - The default speaker output object.
 */
class SApplication
    : public QApplication 
{
    Q_OBJECT
public:
    SApplication( int &argc, char **argv );
    virtual ~SApplication();
    static SApplication &app();

    twSpeaker *getSpeaker() const;
    tw303aEnvironment *get303aEnvironment() const;

    SLink *getCurrentSelectedSLink() const;
    bool isSelectionEmpty() const;
    bool isSLinkSelected( SLink * ) const;
    const SSelectionList &getSelectionList() const;

    SProject *getCurrentProject() const;
    void setCurrentProject( SProject * );
    // Re-fetch the project root component's first output and connect it to
    // the speaker. Call this when the synth graph has changed (tracks
    // added, busses inserted, etc.) so that playback uses the current
    // wiring rather than the snapshot taken at project-creation time.
    void rewireSpeaker();
    offset_t getGlobalLocatorPos() const;
    bool isPlaying() const;
    

signals:
    void globalLocatorMoved( offset_t newPos, offset_t oldPos );

public slots:
    void setSelectedSLink( SLink * );        
    void addSelectedSLink( SLink * );
    void clearSelection();
    void unselectSLink( SLink * );
    void setGlobalLocatorPos( offset_t );
    void setSpeakerMaxVal( sample_t );
    void setPlaying( bool );

private slots:
    void unselectSLink();

private:
    static SApplication *singleton_;
    SSelectionList *selectionList_;
    tw303aEnvironment *t3Env_;
    twSpeaker *t3Speaker_;
    twWhiteNoise *t3WhiteNoise_;

    SLink *currentSelectedSLink_;
    
    offset_t globalLocatorPos_;
    bool isPlaying_;
    SProject *currentProject_;
};

#endif
