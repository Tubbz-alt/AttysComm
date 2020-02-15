#include "AttysCommBase.h"



AttysCommBase::AttysCommBase()
{
	mainThread = NULL;
	adcMuxRegister = new int[2];
	adcMuxRegister[0] = 0;
	adcMuxRegister[1] = 0;
	adcGainRegister = new int[2];
	adcGainRegister[0] = 0;
	adcGainRegister[1] = 0;
	adcCurrNegOn = new int[2];
	adcCurrNegOn[0] = 0;
	adcCurrNegOn[1] = 0;
	adcCurrPosOn = new int[2];
	adcCurrPosOn[0] = 0;
	adcCurrNegOn[1] = 0;

	ringBuffer = new float*[nMem];
	for (int i = 0; i < nMem; i++) {
		ringBuffer[i] = new float[NCHANNELS];
	}

	data = new long[NCHANNELS];
	raw = new char[256];
	sample = new float[NCHANNELS];
	inbuffer = new char[100000];
	strcpy(inbuffer, "");
	doRun = 0;
	inPtr = 0;
	outPtr = 0;
	unregisterCallback();
	adc_rate_index = ADC_DEFAULT_RATE;
	timestamp = 0;
	adc0_gain_index = ADC_GAIN_1;
	adc0_mux_index = ADC_MUX_NORMAL;
	adc1_gain_index = ADC_GAIN_1;
	adc1_mux_index = ADC_MUX_NORMAL;
	accel_full_scale_index = ACCEL_16G;
}


void AttysCommBase::quit() {
	doRun = 0;
	if (mainThread) {
		mainThread->join();
		delete mainThread;
		mainThread = NULL;
		_RPT0(0, "Main acquisition thread shut down.\n");
	}
}


AttysCommBase::~AttysCommBase() {
	quit();
	delete[] adcMuxRegister;
	delete[] adcGainRegister;
	delete[] adcCurrNegOn;
	delete[] adcCurrPosOn;
	for (int i = 0; i < nMem; i++) {
		delete[] ringBuffer[i];
	}
	delete[] ringBuffer;
	delete[] data;
	delete[] raw;
	delete[] sample;
	delete[] inbuffer;
}


/////////////////////////////////////////////////
// ringbuffer keeping data for chunk-wise plotting
float* AttysCommBase::getSampleFromBuffer() {
	if (inPtr != outPtr) {
		float* sample = ringBuffer[outPtr];
		outPtr++;
		if (outPtr == nMem) {
			outPtr = 0;
		}
		return sample;
	}
	else {
		// cause segfault, should never happen
		return nullptr;
	}
}



void AttysCommBase::sendSamplingRate() {
	char tmp[256];
	sprintf(tmp, "r=%d", adc_rate_index);
	sendSyncCommand(tmp, 1);
}

void AttysCommBase::sendFullscaleAccelRange() {
	char tmp[256];
	sprintf(tmp, "t=%d", accel_full_scale_index);
	sendSyncCommand(tmp, 1);
}

void AttysCommBase::sendCurrentMask() {
	char tmp[256];
	sprintf(tmp, "c=%d", current_mask);
	sendSyncCommand(tmp, 1);
}

void AttysCommBase::sendBiasCurrent() {
	char tmp[256];
	sprintf(tmp, "i=%d", current_index);
	sendSyncCommand(tmp, 1);
}

void AttysCommBase::sendGainMux(int channel, int gain, int mux) {
	char tmp[256];
	int v = (mux & 0x0f) | ((gain & 0x0f) << 4);
	switch (channel) {
	case 0:
		sprintf(tmp, "a=%d", v);
		sendSyncCommand(tmp, 1);
		break;
	case 1:
		sprintf(tmp, "b=%d", v);
		sendSyncCommand(tmp, 1);
		break;
	}
	adcGainRegister[channel] = gain;
	adcMuxRegister[channel] = mux;
}

void AttysCommBase::sendInitCommandsToAttys() {
	sendSyncCommand("x=0", 1);
	// switching to base64 encoding
	sendSyncCommand("d=1", 1);
	sendSamplingRate();
	sendFullscaleAccelRange();
	sendGainMux(0, adc0_gain_index, adc0_mux_index);
	sendGainMux(1, adc1_gain_index, adc1_mux_index);
	sendCurrentMask();
	sendBiasCurrent();
	sendSyncCommand("x=1\n", 0);
}

void AttysCommBase::receptionTimeout() {
	_RPT0(0, "Timeout.\n");
	if (attysCommMessage) {
		attysCommMessage->hasMessage(MESSAGE_TIMEOUT, "reception timeout to Attys");
	}
	initialising = 1;
	closeSocket();
	while (doRun) {
		try {
			connect();
			break;
		}
		catch (const char *) {
			_RPT0(0, "Reconnect failed. Sleeping for 1 sec.\n");
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		}
	}
	if (!doRun) {
		return;
	}
	sendInit();
	initialising = 0;
	if (attysCommMessage) {
		attysCommMessage->hasMessage(MESSAGE_RECONNECTED, "reconnected to Attys");
	}
	_RPT0(0, "Reconnected.\n");
}


void AttysCommBase::start() {
	if (mainThread) {
		return;
	}
	mainThread = new std::thread(AttysCommBase::execMainThread, this);
}


void AttysCommBase::processRawAttysData(const char* recvbuffer) {
	watchdogCounter = TIMEOUT_IN_SECS;
	int nTrans = 0;
	char* lf = nullptr;
	strcat(inbuffer, recvbuffer);
	// search for LF (CR is first)
	while ((lf = strchr(inbuffer, 0x0A))) {

		*lf = 0;

		if (strlen(inbuffer) == 29) {

			Base64decode(raw, inbuffer);

			for (int i = 0; i < 2; i++) {
				long v = (raw[i * 3] & 0xff)
					| ((raw[i * 3 + 1] & 0xff) << 8)
					| ((raw[i * 3 + 2] & 0xff) << 16);
				data[INDEX_Analogue_channel_1 + i] = v;
			}

			for (int i = 0; i < 6; i++) {
				long v = (raw[8 + i * 2] & 0xff)
					| ((raw[8 + i * 2 + 1] & 0xff) << 8);
				data[i] = v;
			}

			// Log.d(TAG,""+raw[6]);
			sample[INDEX_GPIO0] = (float)((raw[6] & 32) == 0 ? 0 : 1);
			sample[INDEX_GPIO1] = (float)((raw[6] & 64) == 0 ? 0 : 1);
			isCharging = ((raw[6] & 0x80) == 0 ? 0 : 1);

			// check that the timestamp is the expected one
			int ts = 0;
			nTrans = 1;
			ts = raw[7];
			if ((ts - expectedTimestamp) > 0) {
				if (correctTimestampDifference) {
					nTrans = 1 + (ts - expectedTimestamp);
				}
				else {
					correctTimestampDifference = true;
				}
			}
			// update timestamp
			expectedTimestamp = ++ts;

			// acceleration
			for (int i = INDEX_Acceleration_X;
				i <= INDEX_Acceleration_Z; i++) {
				float norm = 0x8000;
				sample[i] = ((float)data[i] - norm) / norm *
					getAccelFullScaleRange();
			}

			// magnetometer
			for (int i = INDEX_Magnetic_field_X;
				i <= INDEX_Magnetic_field_Z; i++) {
				float norm = 0x8000;
				sample[i] = ((float)data[i] - norm) / norm *
					MAG_FULL_SCALE;
				//Log.d(TAG,"i="+i+","+sample[i]);
			}

			for (int i = INDEX_Analogue_channel_1;
				i <= INDEX_Analogue_channel_2; i++) {
				float norm = 0x800000;
				sample[i] = ((float)data[i] - norm) / norm *
					ADC_REF / ADC_GAIN_FACTOR[adcGainRegister[i
					- INDEX_Analogue_channel_1]];
			}
			// _RPT1(0, "%d\n", data[INDEX_Analogue_channel_1]);


		}
		else {
			_RPT1(0, "Reception error, length=%d, ", (int)strlen(inbuffer));
			_RPT1(0, "recbuffer=>>>%s<<<\n\n,", recvbuffer);
		}

		// in case a sample has been lost
		for (int j = 0; j < nTrans; j++) {
			for (int k = 0; k < NCHANNELS; k++) {
				ringBuffer[inPtr][k] = sample[k];
			}
			if (callbackInterface) {
				callbackInterface->hasSample((float)timestamp, sample);
			}
			timestamp = timestamp + 1.0 / getSamplingRateInHz();
			sampleNumber++;
			inPtr++;
			if (inPtr == nMem) {
				inPtr = 0;
			}
		}

		lf++;
		int rem = (int)strlen(lf) + 1;
		memmove(inbuffer, lf, rem);
	}

}
