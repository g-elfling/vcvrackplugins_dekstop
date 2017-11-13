#include <atomic>
#include <functional>
#include <thread>

#include "dekstop.hpp"
#include "samplerate.h"
#include "../ext/osdialog/osdialog.h"
#include "write_wav.h"
#include "dsp/digital.hpp"
#include "dsp/ringbuffer.hpp"
#include "dsp/frame.hpp"

template <unsigned int ChannelCount>
struct Player : Module {
	enum ParamIds {
		SELECTA_PARAM,
		PLAY_PARAM,
		PLSLOOP_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		TRIG_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		AUDIO1_OUTPUT,
		NUM_OUTPUTS = AUDIO1_OUTPUT + ChannelCount
	};
	enum LightIds {
		PLAYING_LIGHT,
		ITSON_LIGHT,
		NUM_LIGHTS
	};
	
	std::string filename;
	WAV_Writer writer;
	std::atomic_bool wavLoaded;
	std::atomic_bool playing;
	std::atomic_bool hasLooped;

	std::mutex mutex;
	std::thread thread;
	
	std::vector<float> buf;
	int channels;
	int counter;
	int bufsize;
	SchmittTrigger addTrigger;
	SchmittTrigger trigger;

	Player() : Module(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS)
	{
		wavLoaded = false;
		counter = 0;
		bufsize = 0;
		playing = false;
		hasLooped = false;
	}
	~Player();
	void step();
	void clear();
	void startPlaying();
	void stopPlaying();
	void openDialog();
	void playerRun();
	bool readWav();
};

// very nice hi-tech
struct wavfile {
	char id[4];
	int totallength;
	char wavefmt[8];
	int format;
	short pcm;
	short channels;
	int frequency;
	int bytes_per_second;
	short bytes_by_capture;
	short bits_per_sample;
	char data[4];
	int bytes_in_data;
};

template <unsigned int ChannelCount>
Player<ChannelCount>::~Player() {
	if (wavLoaded) thread.join();
}

template <unsigned int ChannelCount>
void Player<ChannelCount>::clear() {
	filename = "";
}

template <unsigned int ChannelCount>
void Player<ChannelCount>::startPlaying() {
	if (wavLoaded) {
		// i don't even know what this means.. just googled some thread thing
		// honestly i have no idea how this thread thing works .........
		if (thread.joinable()) {
			thread.join();
		}
	}
	openDialog();
	if (!filename.empty()) {
		thread = std::thread(&Player<ChannelCount>::playerRun, this);
	}
}

template <unsigned int ChannelCount>
void Player<ChannelCount>::openDialog() {
	std::string dir = filename.empty() ? "." : extractDirectory(filename);
	char *path = osdialog_file(OSDIALOG_OPEN, dir.c_str(), NULL, NULL);
	if (path) {
		filename = path;
		free(path);
	} else {
		filename = "";
	}
	// osdialog_message(OSDIALOG_INFO, OSDIALOG_OK, "Info");
}

// TODO: some day
/*
			char msg[100];
			snprintf(msg, sizeof(msg), "Failed to open WAV file, result = %d\n", result);
			osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, msg);
			fprintf(stderr, "%s", msg);
*/


/*
		snprintf(msg, sizeof(msg), "Failed to close WAV file, result = %d\n", result);
		osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, msg);
		fprintf(stderr, "%s", msg);
*/

template <unsigned int ChannelCount>
bool Player<ChannelCount>::readWav() {
	// only works with 16 bit 44100 hz wav......cud just use libsndfile ...??
	FILE *fp;
	struct wavfile header;

	fp = fopen(filename.c_str(), "rb");
	if (fp == NULL) {
		fprintf(stderr, "Not able to open input file %s.\n", filename.c_str());
		return false;
	}

	// read header
	if (fread(&header, sizeof(header), 1, fp) < 1) {
		fprintf(stderr, "Can't read file header\n");
		return false;
	}

	if (strncmp(header.id, "RIFF", 4) != 0 ||
			strncmp(header.data, "data", 4) != 0 ||
			strncmp(header.wavefmt, "WAVEfmt", 7) != 0) {
		fprintf(stderr, "file is not wav\n"); 
		return false;
	}

	channels = header.channels;

	int frames = header.bytes_in_data / channels / 2;

	int size = frames * channels;

	// my poor computer segfault if too big size :(
	// 2646000 is ok but double that too big..30 seconds works...too long big no..sorry big files
	if (size > 2646000) size = 2646000;

	int16_t buffer[size];
	if ((fread(buffer, sizeof(buffer), 1, fp)) != 1) {
		fprintf(stdout, "dunno what happend!!! buffer size was supposed to be OK!!! sry :("); 
		return false;
	}

	buf.clear();
	buf.resize(size);

	bufsize = buf.size();
	for (int k = 0; k < size; k++) {
		float s = (float) buffer[k] / INT16_MAX;
		buf[k] = s;
	}

	fclose(fp);
	return true;
}

template <unsigned int ChannelCount>
void Player<ChannelCount>::playerRun() {
	if (readWav() == true) {
		wavLoaded = true;
		counter = 0; // start playing from the beginning???
		playing = true;
	}
}

template <unsigned int ChannelCount>
void Player<ChannelCount>::step() {
	if (addTrigger.process(params[PLAY_PARAM].value) || trigger.process(inputs[TRIG_INPUT].value) || inputs[TRIG_INPUT].value >= 0.7) {
		counter = 0;
		playing = true;
	}

	lights[PLAYING_LIGHT].value = wavLoaded ? 1.0 : 0.0;
	lights[ITSON_LIGHT].value = 0.0;

	if (wavLoaded && playing) {
		lights[ITSON_LIGHT].value = 1.0;
		if (!(hasLooped && params[PLSLOOP_PARAM].value == 0.0)) {
			for (int i = 0; i < ChannelCount; i++) {
				if (i + 1 <= channels) {
					outputs[AUDIO1_OUTPUT + i].value = buf[counter + i] * 5.0;
				}
			}

			// lol cud someone PLEASE open bull request and tell me how to play music
			// WITHOUT this horrible buffer++ kinda thing...pretty please!!!!	
			counter += channels;

			if (counter >= bufsize) {
				counter = 0;
				hasLooped = true;
			}
		}
	}
}

struct PlayButton : LEDButton {
	using Callback = std::function<void()>;

	Callback onPressCallback;
	SchmittTrigger playTrigger;
	
	void onChange(EventChange &e) override {
		if (playTrigger.process(value)) {
			onPress(e);
		}
	}
	void onPress(EventChange &e) {
		assert (onPressCallback);
		onPressCallback();
	}
};


template <unsigned int ChannelCount>
PlayerWidget<ChannelCount>::PlayerWidget() {
	Player<ChannelCount> *module = new Player<ChannelCount>();
	setModule(module);
	// dunno why there was a +5 ???
	box.size = Vec(15*6, 380);

	{
		Panel *panel = new LightPanel();
		panel->box.size = box.size;
		addChild(panel);
	}

	float margin = 5;
	float labelHeight = 15;
	float yPos = margin;
	float xPos = margin;

	{
		Label *label = new Label();
		label->box.pos = Vec(xPos, yPos);
		label->text = "Play'YAH " + std::to_string(ChannelCount);
		addChild(label);
		yPos += labelHeight + margin;

		xPos = 35;
		yPos += 2*margin;
		ParamWidget *playButton = createParam<PlayButton>(Vec(xPos, yPos-1), module, Player<ChannelCount>::SELECTA_PARAM, 0.0, 1.0, 0.0);
		PlayButton *btn = dynamic_cast<PlayButton*>(playButton);
		Player<ChannelCount> *player = dynamic_cast<Player<ChannelCount>*>(module);

		btn->onPressCallback = [=]()
		{
			player->startPlaying();
		};
		addParam(playButton);
		addChild(createLight<SmallLight<GreenLight>>(Vec(xPos+6, yPos+5), module, Player<ChannelCount>::PLAYING_LIGHT));
		xPos = margin;
		yPos += playButton->box.size.y + 3*margin;
	}


	// very nice Elements style pushie very nice
	addParam(createParam<CKD6>(Vec(12, 290), module, Player<ChannelCount>::PLAY_PARAM, 0.0, 1.0, 0.0));

	// gui placement not my strongest suit
	// but gotta place these "down down"
	// because 8 channel wav player takes very much space
	addInput(createInput<PJ301MPort>(Vec(55, 290), module, Player<ChannelCount>::TRIG_INPUT));
	addChild(createLight<MediumLight<GreenLight>>(Vec(20, 335), module, Player<ChannelCount>::ITSON_LIGHT));
	addParam(createParam<CKSS>(Vec(62, 330), module, Player<ChannelCount>::PLSLOOP_PARAM, 0.0, 1.0, 0.0));


	{
		Label *label = new Label();
		label->box.pos = Vec(margin, yPos);
		label->text = "Channels";
		addChild(label);
		yPos += labelHeight + margin;
	}

	yPos += 5;
	xPos = 10;
	for (int i = 0; i < ChannelCount; i++) {
		addOutput(createOutput<PJ3410Port>(Vec(xPos, yPos), module, i));
		Label *label = new Label();
		label->box.pos = Vec(xPos + 4, yPos + 28);
		label->text = stringf("%d", i + 1);
		addChild(label);

		if (i % 2 ==0) {
			xPos += 37 + margin;
		} else {
			xPos = 10;
			yPos += 40 + margin;
		}
	}
}

Player2Widget::Player2Widget() :
	PlayerWidget<2u>()
{
}

Player8Widget::Player8Widget() :
	PlayerWidget<8u>()
{
}
