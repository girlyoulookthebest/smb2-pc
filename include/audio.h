#ifndef AUDIO_H
#define AUDIO_H

#include "platform.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MusyX sound parameter structures (simplified for PC) */
typedef struct {
    uint16_t ctrl;
    union {
        int32_t value14;
        int16_t value7;
        float   fvalue;
    } paraData;
} SND_PARAMETER;

typedef struct {
    uint8_t numPara;
    SND_PARAMETER* paraArray;
} SND_PARAMETER_INFO;

/* Sound effect flags (matching original) */
#define SND_FLAG_VOLUME_UNMANAGED  0x0001
#define SND_FLAG_LEFT_CHANNEL      0x0002
#define SND_FLAG_RIGHT_CHANNEL     0x0004
#define SND_FLAG_8000              0x8000

/* Sound group description */
typedef struct {
    char* name;
    int32_t firstSoundId;
    int32_t groupId;
    int32_t slot;
    char* baseName;
} SoundGroupDesc;

/* Sound effect description */
typedef struct {
    uint16_t fxId;
    uint16_t unk2;
    char* name;
    uint16_t flags;
    int16_t repeatDelay;
    int16_t cueId;
    uint16_t unkE;
} SoundEffectDesc;

/* Streamed music (ADX) */
typedef struct {
    char* name;
    uint8_t volume;
    char* file;
    float frameRate;
    uint8_t unk10;  /* 2: mono, 4: not music, 8: alias */
} SelWorldSound;

/* Audio system API */
bool audio_init(void);
void audio_shutdown(void);
void audio_update(void);  /* Call once per frame */

/* MusyX compatibility functions */
int32_t sndPushGroup(void* projData, int32_t groupId, void* sampData, void* sdirData, void* poolData);
void sndPopGroup(int32_t groupId);
uint32_t sndFXStartParaInfo(uint16_t fxId, int32_t volume, int32_t pan, int32_t surround, SND_PARAMETER_INFO* info);
void sndFXKeyOff(uint32_t voiceId);
void sndFXSetCtrl(uint32_t voiceId, uint16_t ctrl, int32_t value);

/* High-level sound API (matching original game) */
void SoundGroupLoad(int groupId);
void g_something_calls_sndPopGroup(void);
void g_smth_calls_sndFXStartParaInfo(void* req);
int32_t SoundReqDirect(uint32_t soundId);
void SoundOff(uint16_t soundId);
void SoundVol(uint16_t soundId, uint8_t volume);
void SoundPan(uint16_t soundId, int8_t pan, int8_t span);
void SoundPitch(uint16_t soundId, int16_t pitch);
void SoftStreamSEReq(uint32_t soundId);
void g_crossfade_music(void);

/* Volume control */
void audio_set_master_volume(float volume);
void audio_set_music_volume(float volume);
void audio_set_sfx_volume(float volume);
/* mixer category volume 0..1 (ADSP_CAT_* in audio_dsp.h) */
void audio_set_category_volume(int cat, float volume);

/* Audio device enumeration */
typedef struct {
    int id;
    char name[256];
    bool is_default;
} AudioDeviceInfo;

int audio_get_device_count(void);
const AudioDeviceInfo* audio_get_device_info(int index);
bool audio_set_output_device(int device_id);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */