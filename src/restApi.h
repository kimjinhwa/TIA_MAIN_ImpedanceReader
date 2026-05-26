#ifndef REST_API_H
#define REST_API_H

#ifdef WIFI_AP_MODE
void restApiInit(void);
void restApiHandle(void);
bool restApiIsUploadInProgress(void);
#endif

#endif
