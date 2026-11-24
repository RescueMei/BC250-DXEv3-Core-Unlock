#ifndef __MEIMEIDXEV3_CONFIG_VAR_H__
#define __MEIMEIDXEV3_CONFIG_VAR_H__

#include <Uefi.h>
#include <Config/MeiMeiDXEv3Config.h>

#define MEIMEIDXEV3_FACTORY_MASK_VAR_NAME  L"MeiMeiDXEv3CoreFactoryMaskVar"
#define MEIMEIDXEV3_LEARN_FLAG_VAR_NAME    L"MeiMeiDXEv3LearnFactoryFlag"
#define MEIMEIDXEV3_LEARN_RETRY_VAR_NAME   L"MeiMeiDXEv3CoreLearnRetries"
#define MEIMEIDXEV3_MAX_LEARN_ATTEMPTS     3U

typedef struct {
  UINT8   FactoryMask;
  UINT8   Reserved[3];
  UINT32  OriginalCrc32;
} FACTORY_MASK_RECORD;

#endif
