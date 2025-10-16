const audio = document.getElementById('audioPlayer');
const playBtn = document.getElementById('playBtn');
const playIcon = document.getElementById('playIcon');
const stopIcon = document.getElementById('stopIcon');
const volumeSlider = document.getElementById('volumeSlider');
const volumeValue = document.getElementById('volumeValue');
const volumeIcon = document.getElementById('volumeIcon');
const statusText = document.getElementById('statusText');
const statusDot = document.getElementById('statusDot');
const latencyInfo = document.getElementById('latencyInfo');
const formatValue = document.getElementById('formatValue');
const bitrateValue = document.getElementById('bitrateValue');
const streamSubtitle = document.getElementById('streamSubtitle');

const streamUrl = '/stream';

let isPlaying = false;
let volumeTimeout;
let isMuted = false;
let previousVolume = 80;
let mediaSource = null;
let sourceBuffer = null;
let reader = null;
let latencyUpdateInterval = null;
let streamMimeType = null;
let lastKnownFormat = '-';
let lastKnownBitrate = '-';
let streamTitle = 'Live Audio Stream';

const savedVolume = localStorage.getItem('netcast_volume') || 80;
volumeSlider.value = savedVolume;
audio.volume = savedVolume / 100;
volumeValue.textContent = savedVolume + '%';
previousVolume = savedVolume;

function decodeUTF8Header(headerValue) {
	if (!headerValue) return '';
	
	try {
		if (!/[\x80-\xFF]/.test(headerValue)) {
			return headerValue;
		}
		
		const bytes = new Uint8Array(headerValue.length);
		for (let i = 0; i < headerValue.length; i++) {
			bytes[i] = headerValue.charCodeAt(i) & 0xFF;
		}
		
		const decoder = new TextDecoder('utf-8');
		return decoder.decode(bytes);
	} catch (e) {
		console.warn('UTF-8 decode failed:', e);
		return headerValue;
	}
}

function updateVolumeIcon(volume) {
	let iconName;
	if (volume == 0 || isMuted) {
		iconName = 'audio-volume-muted.svg';
	} else if (volume <= 33) {
		iconName = 'audio-volume-low.svg';
	} else if (volume <= 66) {
		iconName = 'audio-volume-medium.svg';
	} else {
		iconName = 'audio-volume-high.svg';
	}
	volumeIcon.src = '/resource/' + iconName;
}

updateVolumeIcon(savedVolume);

function updateLatencyInfo() {
	if (!isPlaying) {
		latencyInfo.textContent = '';
		return;
	}
	
	if (sourceBuffer && sourceBuffer.buffered.length > 0) {
		const buffered = sourceBuffer.buffered.end(0) - audio.currentTime;
		const latency = Math.round(buffered * 1000);
		latencyInfo.textContent = `${buffered.toFixed(1)}s | ${latency}ms`;
	} else if (audio.buffered.length > 0 && audio.currentTime > 0) {
		const buffered = audio.buffered.end(0) - audio.currentTime;
		const latency = Math.round(buffered * 1000);
		latencyInfo.textContent = `${buffered.toFixed(1)}s | ${latency}ms`;
	}
}

function updateStreamInfo(headers) {
	const contentType = headers.get('content-type');
	const icyBr = headers.get('icy-br');
	const icyName = headers.get('icy-name');
	const xSamplerate = headers.get('x-audio-samplerate');
	const xChannels = headers.get('x-audio-channels');
	const xBitrate = headers.get('x-audio-bitrate');
	const xBitdepth = headers.get('x-audio-bitdepth');
	
	if (icyName) {
		streamTitle = decodeUTF8Header(icyName);
		streamSubtitle.textContent = streamTitle;
		document.title = streamTitle + ' - NetCast';
	}
	
	if (contentType) {
		const mainType = contentType.split(';')[0].trim();
		
		if (mainType.includes('audio/mpeg')) {
			if (xSamplerate && xChannels) {
				const rateKhz = (parseInt(xSamplerate) / 1000).toFixed(1);
				const channelName = parseInt(xChannels) === 2 ? 'Stereo' : 'Mono';
				lastKnownFormat = `MP3 ${rateKhz}kHz ${channelName}`;
			} else {
				lastKnownFormat = 'MP3';
			}
		} else if (mainType.includes('audio/wav') || mainType.includes('audio/wave') || mainType.includes('audio/x-wav')) {
			if (xSamplerate && xChannels && xBitdepth) {
				const rateKhz = (parseInt(xSamplerate) / 1000).toFixed(1);
				const channelName = parseInt(xChannels) === 2 ? 'Stereo' : 'Mono';
				lastKnownFormat = `PCM ${xBitdepth}-bit ${rateKhz}kHz ${channelName}`;
			} else {
				lastKnownFormat = 'PCM';
			}
		} else {
			const match = mainType.match(/audio\/([a-z0-9-]+)/i);
			lastKnownFormat = match ? match[1].toUpperCase() : '-';
		}
	} else {
		lastKnownFormat = 'Unknown Format';
	}
	
	formatValue.textContent = lastKnownFormat;
	
	if (xBitrate) {
		lastKnownBitrate = xBitrate + ' kbps';
	} else if (icyBr) {
		lastKnownBitrate = icyBr + ' kbps';
	} else {
		lastKnownBitrate = '-';
	}
	
	bitrateValue.textContent = lastKnownBitrate;
}

function startStream() {
	formatValue.textContent = 'Connecting...';
	bitrateValue.textContent = 'Connecting...';
	
	const fetchTimeout = setTimeout(() => {
		formatValue.textContent = 'Connection timeout';
		bitrateValue.textContent = 'Please retry';
		statusText.textContent = 'Connection timeout';
		stopStream();
	}, 10000);
	
	fetch(streamUrl)
		.then(response => {
			clearTimeout(fetchTimeout);
			
			if (!response.ok) throw new Error('Stream connection failed');
			
			updateStreamInfo(response.headers);
			
			const contentType = response.headers.get('content-type') || '';
			streamMimeType = contentType.split(';')[0].trim();
			
			if ('MediaSource' in window && streamMimeType.includes('mpeg')) {
				startMediaSourceStream(response);
			} else {
				fallbackStream();
			}
		})
		.catch(error => {
			clearTimeout(fetchTimeout);
			formatValue.textContent = lastKnownFormat !== '-' ? lastKnownFormat : 'Connection failed';
			bitrateValue.textContent = lastKnownBitrate !== '-' ? lastKnownBitrate : 'Error';
			statusText.textContent = 'Connection error';
			stopStream();
		});
}

function startMediaSourceStream(response) {
	mediaSource = new MediaSource();
	audio.src = URL.createObjectURL(mediaSource);

	mediaSource.addEventListener('sourceopen', function() {
		try {
			sourceBuffer = mediaSource.addSourceBuffer(streamMimeType);
			sourceBuffer.mode = 'sequence';

			reader = response.body.getReader();
			pump();
		} catch (error) {
			fallbackStream();
		}
	});

	function pump() {
		reader.read().then(({ done, value }) => {
			if (done) {
				if (mediaSource.readyState === 'open') {
					mediaSource.endOfStream();
				}
				return;
			}

			if (sourceBuffer.buffered.length > 0 && audio.currentTime > 0) {
				const buffered = sourceBuffer.buffered.end(0) - audio.currentTime;
				if (buffered > 1.2) {
					audio.currentTime = sourceBuffer.buffered.end(0) - 0.4;
				}
			}

			if (!sourceBuffer.updating) {
				try {
					sourceBuffer.appendBuffer(value);
				} catch (error) {
				}
			}

			if (sourceBuffer.updating) {
				sourceBuffer.addEventListener('updateend', pump, { once: true });
			} else {
				pump();
			}
		}).catch(error => {
			if (isPlaying) {
				statusText.textContent = 'Stream interrupted';
			}
		});
	}
	
	latencyUpdateInterval = setInterval(updateLatencyInfo, 100);
	
	const playPromise = audio.play();
	if (playPromise !== undefined) {
		playPromise.catch(e => {
			statusText.textContent = 'Play failed';
			statusDot.classList.add('stopped');
			isPlaying = false;
			playIcon.style.display = 'block';
			stopIcon.style.display = 'none';
		});
	}
}

function fallbackStream() {
	audio.src = streamUrl;
	latencyUpdateInterval = setInterval(updateLatencyInfo, 100);
	
	const playPromise = audio.play();
	if (playPromise !== undefined) {
		playPromise.catch(e => {
			statusText.textContent = 'Play failed';
			statusDot.classList.add('stopped');
			isPlaying = false;
			playIcon.style.display = 'block';
			stopIcon.style.display = 'none';
		});
	}
}

function stopStream() {
	audio.pause();
	audio.src = '';
	
	if (reader) {
		reader.cancel();
		reader = null;
	}
	
	if (mediaSource && mediaSource.readyState === 'open') {
		mediaSource.endOfStream();
	}
	
	if (latencyUpdateInterval) {
		clearInterval(latencyUpdateInterval);
		latencyUpdateInterval = null;
	}
	
	mediaSource = null;
	sourceBuffer = null;
	isPlaying = false;
	
	playIcon.style.display = 'block';
	stopIcon.style.display = 'none';
	statusText.textContent = 'Stopped';
	statusDot.classList.add('stopped');
	
	formatValue.textContent = lastKnownFormat;
	bitrateValue.textContent = lastKnownBitrate;
	latencyInfo.textContent = '';
}

playBtn.addEventListener('click', () => {
	if (!isPlaying) {
		isPlaying = true;
		startStream();
	} else {
		stopStream();
	}
});

audio.addEventListener('play', () => {
	isPlaying = true;
	playIcon.style.display = 'none';
	stopIcon.style.display = 'block';
	statusText.textContent = 'Playing';
	statusDot.classList.remove('stopped');
});

audio.addEventListener('pause', () => {
	if (isPlaying && audio.src) {
		statusText.textContent = 'Buffering...';
	}
});

audio.addEventListener('waiting', () => {
	if (isPlaying) {
		statusText.textContent = 'Buffering...';
	}
});

audio.addEventListener('canplay', () => {
	if (isPlaying) {
		statusText.textContent = 'Playing';
	}
});

audio.addEventListener('error', (e) => {
	if (isPlaying) {
		statusText.textContent = 'Connection error';
	}
});

audio.addEventListener('ended', () => {
	stopStream();
});

volumeSlider.addEventListener('input', (e) => {
	const value = e.target.value;
	volumeValue.textContent = value + '%';
	
	if (!isMuted) {
		previousVolume = value;
	}
	
	updateVolumeIcon(value);
	
	clearTimeout(volumeTimeout);
	volumeTimeout = setTimeout(() => {
		audio.volume = value / 100;
		localStorage.setItem('netcast_volume', value);
	}, 50);
});

volumeIcon.addEventListener('click', () => {
	if (isMuted) {
		isMuted = false;
		volumeSlider.value = previousVolume;
		audio.volume = previousVolume / 100;
		volumeValue.textContent = previousVolume + '%';
		updateVolumeIcon(previousVolume);
	} else {
		isMuted = true;
		previousVolume = volumeSlider.value;
		volumeSlider.value = 0;
		audio.volume = 0;
		volumeValue.textContent = '0%';
		updateVolumeIcon(0);
	}
});

document.addEventListener('keydown', (e) => {
	if (e.code === 'Space' && e.target.tagName !== 'INPUT') {
		e.preventDefault();
		playBtn.click();
	}
});
