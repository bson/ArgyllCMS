/* ColorSync declarations mising from Apple SDK's... */

#ifndef __OLDCOLORSYNC_H__
#define __OLDCOLORSYNC_H__

extern CMError
CMGetDeviceDefaultProfileID(
  CMDeviceClass        deviceClass,
  CMDeviceID           deviceID,
  CMDeviceProfileID *  defaultProfID);

extern CMError
CMGetDeviceProfile(
  CMDeviceClass        deviceClass,
  CMDeviceID           deviceID,
  CMDeviceProfileID    profileID,
  CMProfileLocation *  profileLoc);

extern CMError
CMOpenProfile(
  CMProfileRef *             prof,
  const CMProfileLocation *  theProfile);

extern CMError
CMGetProfileByAVID(
  CMDisplayIDType   theID,
  CMProfileRef *    prof);

extern CMError
NCMGetProfileLocation(
  CMProfileRef         prof,
  CMProfileLocation *  theProfile,
  UInt32 *             locationSize);

extern CMError
CMCopyProfile(
  CMProfileRef *             targetProf,
  const CMProfileLocation *  targetLocation,
  CMProfileRef               srcProf);

extern CMError
CMCloseProfile(CMProfileRef prof);

extern CMError
CMSetProfileDescriptions(
  CMProfileRef       prof,
  const char *       aName,
  UInt32             aCount,
  ConstStr255Param   mName,
  ScriptCode         mCode,
  const UniChar *    uName,
  UniCharCount       uCount);

extern CMError
CMSetProfileElement(
  CMProfileRef   prof,
  OSType         tag,
  UInt32         elementSize,
  const void *   elementData);

extern CMError
CMUpdateProfile(CMProfileRef prof);

extern CMError
CMSetProfileByAVID(
  CMDisplayIDType   theID,
  CMProfileRef      prof);

extern CMError
CMGetProfileHeader(
  CMProfileRef            prof,
  CMAppleProfileHeader *  header);

/* From CGDirectDisplay.h */
extern  size_t CGDisplayBitsPerSample(CGDirectDisplayID display);

#endif /* __OLDCOLORSYNC_H__ */
