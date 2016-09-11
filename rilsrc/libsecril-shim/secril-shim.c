#include "secril-shim.h"

/* A copy of the original RIL function table. */
static const RIL_RadioFunctions *origRilFunctions;

/* A copy of the ril environment passed to RIL_Init. */
static const struct RIL_Env *rilEnv;

static void onRequestDial(int request, void *data, size_t datalen, RIL_Token t) {
	RIL_Dial *dial = malloc(sizeof(RIL_Dial));
	RIL_UUS_Info *uusInfo = malloc(sizeof(RIL_UUS_Info));

	memcpy(dial, data, datalen);

	if (dial->uusInfo == NULL) {
		memset(uusInfo, 0, sizeof(RIL_UUS_Info));
		uusInfo->uusType = (RIL_UUS_Type) 0;
		uusInfo->uusDcs = (RIL_UUS_DCS) 0;
		uusInfo->uusData = NULL;
		uusInfo->uusLength = 0;
		dial->uusInfo = uusInfo;
	}

	origRilFunctions->onRequest(request, dial, sizeof(RIL_Dial), t);

	free(uusInfo);
	free(dial);
}

static void onRequestShim(int request, void *data, size_t datalen, RIL_Token t)
{
	switch (request) {
		/* The Samsung RIL crashes if uusInfo is NULL... */
		case RIL_REQUEST_DIAL:
			if (datalen == sizeof(RIL_Dial) && data != NULL) {
				onRequestDial(request, data, datalen, t);
				return;
			}
			break;
	}

	RLOGD("%s: got request %s: forwarded to RIL.\n", __func__, requestToString(request));
	origRilFunctions->onRequest(request, data, datalen, t);
}

static void onCompleteRequestGetSimStatus(RIL_Token t, RIL_Errno e, void *response) {
	/* While at it, upgrade the response to RIL_CardStatus_v6 */
	RIL_CardStatus_v5_samsung *p_cur = ((RIL_CardStatus_v5_samsung *) response);
	RIL_CardStatus_v6 *v6response = malloc(sizeof(RIL_CardStatus_v6));

	v6response->card_state = p_cur->card_state;
	v6response->universal_pin_state = p_cur->universal_pin_state;
	v6response->gsm_umts_subscription_app_index = p_cur->gsm_umts_subscription_app_index;
	v6response->cdma_subscription_app_index = p_cur->cdma_subscription_app_index;
	v6response->ims_subscription_app_index = -1;
	v6response->num_applications = p_cur->num_applications;

	int i;
	for (i = 0; i < RIL_CARD_MAX_APPS; ++i)
		memcpy(&v6response->applications[i], &p_cur->applications[i], sizeof(RIL_AppStatus));

	/* Send the fixed response to libril */
	rilEnv->OnRequestComplete(t, e, v6response, sizeof(RIL_CardStatus_v6));

	free(v6response);
}

static void fixupDataCallList(void *response, size_t responselen) {
	RIL_Data_Call_Response_v6 *p_cur = (RIL_Data_Call_Response_v6 *) response;
	int num = responselen / sizeof(RIL_Data_Call_Response_v6);

	int i;
	for (i = 0; i < num; ++i)
		p_cur[i].gateways = p_cur[i].addresses;
}

static void onCompleteQueryAvailableNetworks(RIL_Token t, RIL_Errno e, void *response, size_t responselen) {
	/* Response is a char **, pointing to an array of char *'s */
	size_t numStrings = responselen / sizeof(char *);
	size_t numNeededStrings = numStrings - (numStrings / 5);
	size_t newResponseLen = numNeededStrings * sizeof(char *);

	void *newResponse = malloc(newResponseLen);

	/* Remove every 5th string (qan element) */
	char **p_cur = (char **) response;
	char **p_new = (char **) newResponse;
	size_t i, j;
	for (i = 0, j = 0; i < numStrings; ++i) {
		if ((i + 1) % 5 != 0) {
			p_new[j] = p_cur[i];
			++j;
		}
	}

	/* Send the fixed response to libril */
	rilEnv->OnRequestComplete(t, e, newResponse, newResponseLen);

	free(newResponse);
}

static void fixupSignalStrength(void *response) {
	int gsmSignalStrength;

	RIL_SignalStrength_v8 *p_cur = ((RIL_SignalStrength_v8 *) response);

	gsmSignalStrength = p_cur->GW_SignalStrength.signalStrength & 0xFF;

	if (gsmSignalStrength < 0 ||
		(gsmSignalStrength > 31 && p_cur->GW_SignalStrength.signalStrength != 99)) {
		gsmSignalStrength = p_cur->CDMA_SignalStrength.dbm;
	}

	/* Fix GSM signal strength */
	p_cur->GW_SignalStrength.signalStrength = gsmSignalStrength;

	/* We don't support LTE - values should be set to INT_MAX */
	p_cur->LTE_SignalStrength.cqi = INT_MAX;
	p_cur->LTE_SignalStrength.rsrp = INT_MAX;
	p_cur->LTE_SignalStrength.rsrq = INT_MAX;
	p_cur->LTE_SignalStrength.rssnr = INT_MAX;
}

static void onRequestCompleteShim(RIL_Token t, RIL_Errno e, void *response, size_t responselen) {
	int request;
	RequestInfo *pRI;

	pRI = (RequestInfo *)t;

	/* If pRI is null, this entire function is useless. */
	if (pRI == NULL)
		goto null_token_exit;

	request = pRI->pCI->requestNumber;

	switch (request) {
		case RIL_REQUEST_GET_SIM_STATUS:
			/* Remove unused extra elements from RIL_AppStatus */
			if (response != NULL && responselen == sizeof(RIL_CardStatus_v5_samsung)) {
				onCompleteRequestGetSimStatus(t, e, response);
				return;
			}
			break;
		case RIL_REQUEST_DATA_CALL_LIST:
		case RIL_REQUEST_SETUP_DATA_CALL:
			/* According to the Samsung RIL, the addresses are the gateways?
			 * This fixes mobile data. */
			if (response != NULL && responselen != 0 && (responselen % sizeof(RIL_Data_Call_Response_v6) == 0)) {
				fixupDataCallList(response, responselen);
				rilEnv->OnRequestComplete(t, e, response, responselen);
				return;
			}
			break;
		case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS:
			/* Remove the extra (unused) element from the operator info, freaking out the framework.
			 * Formerly, this is know as the mQANElements override. */
			if (response != NULL && responselen != 0 && (responselen % sizeof(char *) == 0)) {
				onCompleteQueryAvailableNetworks(t, e, response, responselen);
				return;
			}
			break;
		case RIL_REQUEST_SIGNAL_STRENGTH:
			/* The Samsung RIL reports the signal strength in a strange way... */
			if (response != NULL && responselen >= sizeof(RIL_SignalStrength_v5)) {
				fixupSignalStrength(response);
				rilEnv->OnRequestComplete(t, e, response, responselen);
				return;
			}
			break;
	}

	RLOGD("%s: got request %s: forwarded to libril.\n", __func__, requestToString(request));
null_token_exit:
	rilEnv->OnRequestComplete(t, e, response, responselen);
}

static void onUnsolicitedResponseShim(int unsolResponse, const void *data, size_t datalen)
{
	switch (unsolResponse) {
		case RIL_UNSOL_DATA_CALL_LIST_CHANGED:
			/* According to the Samsung RIL, the addresses are the gateways?
			 * This fixes mobile data. */
			if (data != NULL && datalen != 0 && (datalen % sizeof(RIL_Data_Call_Response_v6) == 0))
				fixupDataCallList((void*) data, datalen);
			break;
		case RIL_UNSOL_SIGNAL_STRENGTH:
			/* The Samsung RIL reports the signal strength in a strange way... */
			if (data != NULL && datalen >= sizeof(RIL_SignalStrength_v5))
				fixupSignalStrength((void*) data);
			break;
	}

	rilEnv->OnUnsolicitedResponse(unsolResponse, data, datalen);
}

const RIL_RadioFunctions* RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
	RIL_RadioFunctions const* (*origRilInit)(const struct RIL_Env *env, int argc, char **argv);
	static RIL_RadioFunctions shimmedFunctions;
	static struct RIL_Env shimmedEnv;
	void *origRil;

	/* Shim the RIL_Env passed to the real RIL, saving a copy of the original */
	rilEnv = env;
	shimmedEnv = *env;
	shimmedEnv.OnRequestComplete = onRequestCompleteShim;
	shimmedEnv.OnUnsolicitedResponse = onUnsolicitedResponseShim;

	/* Open and Init the original RIL. */

	origRil = dlopen(RIL_LIB_PATH, RTLD_LOCAL);
	if (CC_UNLIKELY(!origRil)) {
		RLOGE("%s: failed to load '" RIL_LIB_PATH  "': %s\n", __func__, dlerror());
		return NULL;
	}

	origRilInit = dlsym(origRil, "RIL_Init");
	if (CC_UNLIKELY(!origRilInit)) {
		RLOGE("%s: couldn't find original RIL_Init!\n", __func__);
		goto fail_after_dlopen;
	}

	origRilFunctions = origRilInit(&shimmedEnv, argc, argv);
	if (CC_UNLIKELY(!origRilFunctions)) {
		RLOGE("%s: the original RIL_Init derped.\n", __func__);
		goto fail_after_dlopen;
	}

	/* Shim functions as needed. */
	shimmedFunctions = *origRilFunctions;
	shimmedFunctions.onRequest = onRequestShim;

	return &shimmedFunctions;

fail_after_dlopen:
	dlclose(origRil);
	return NULL;
}
