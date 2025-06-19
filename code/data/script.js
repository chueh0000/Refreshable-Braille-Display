// This array will be populated by the server initially or fetched via API
let electromagnetsData = [];
let refreshIntervalId; // To store the interval ID for clearing later

// New: Pattern related variables
// Each frame is now an object { emStates: [{isOn, isReversed}], duration: number }
let currentPattern = []; 
let currentPatternFrameIndex = 0;
let patternPlaybackTimeout = null; // Use setTimeout for variable durations

/**
 * Toggles the state of an electromagnet (ON/OFF).
 * When turning OFF, it triggers a 0.1-second reverse current.
 * @param {number} index - The index of the electromagnet to toggle.
 */
function toggleElectromagnet(index) {
  // If a pattern is playing, stop it first to allow manual control
  if (patternPlaybackTimeout) {
      stopPattern();
  }

  const em = electromagnetsData[index];
  const newState = em.isOn ? 'off' : 'on';
  let newDuration = em.durationMs;
  // For 'on' state, use the current polarity. For 'off' state, the ESP32 will handle
  // the reversal based on the stored 'isReversed' property.
  let newReversed = em.isReversed; 

  if (newState === 'off') {
    newDuration = 100; // Still send 0.1 second duration for the pulse
    // Do NOT change newReversed here. It should remain the last ON polarity,
    // so the ESP32 knows what polarity to reverse from.
  }

  // Send command to ESP32
  sendCommand(index, newState, newReversed, newDuration);
}

/**
 * Toggles the polarity of an electromagnet.
 * If the electromagnet is currently ON, its polarity will change immediately.
 * If OFF, the stored polarity state will be updated for future use.
 * @param {number} index - The index of the electromagnet to toggle polarity for.
 */
function togglePolarity(index) {
    // If a pattern is playing, stop it first to allow manual control
    if (patternPlaybackTimeout) {
        stopPattern();
    }

    const em = electromagnetsData[index];
    const newReversed = !em.isReversed; // Toggle the current polarity state for display and future ON.
    // Send a command to the ESP32 to just toggle polarity.
    // We'll use a new 'state' parameter, e.g., 'toggle_polarity'
    // The ESP32 will update its internal state and re-apply current if already ON.
    sendCommand(index, 'toggle_polarity', newReversed, em.durationMs);
}

/**
 * Sets the duration for which an electromagnet stays ON.
 * If the electromagnet is currently ON, the new duration will take effect immediately.
 * @param {number} index - The index of the electromagnet.
 */
function setDuration(index) {
  // If a pattern is playing, stop it first to allow manual control
  if (patternPlaybackTimeout) {
      stopPattern();
  }

  const durationInput = document.getElementById('duration_' + index);
  const newDuration = parseInt(durationInput.value);
  const em = electromagnetsData[index];
  em.durationMs = newDuration; // Update local data immediately
  if (em.isOn) { // If already on, re-send command to update duration on ESP32
    sendCommand(index, 'on', em.isReversed, em.durationMs);
  }
}

/**
 * Updates the UI elements for a specific electromagnet based on its current state.
 * @param {number} index - The index of the electromagnet.
 * @param {boolean} isOn - Whether the electromagnet is currently ON.
 * @param {boolean} isReversed - Whether the electromagnet's polarity is reversed.
 * @param {number} durationMs - The duration in milliseconds.
 * @param {boolean} isReversingOff - Whether the electromagnet is currently in the 0.1-sec reverse-off state.
 */
function updateUI(index, isOn, isReversed, durationMs, isReversingOff) {
  const dot = document.getElementById('dot_' + index);
  const statusText = document.getElementById('status_text_' + index);
  const polarityStatus = document.getElementById('polarity_status_' + index);
  const durationInput = document.getElementById('duration_' + index);
  const polarityToggleButton = document.getElementById('polarity_toggle_btn_' + index);

  if (!dot || !statusText || !polarityStatus || !durationInput || !polarityToggleButton) {
      // console.warn(`UI elements for EM ${index} not found. Skipping UI update.`);
      return; // Exit if elements are not ready (e.g., during initial pattern loading or playback where individual controls are not visible)
  }

  // Determine dot class based on state
  let dotClass = 'dot ';
  if (isReversingOff) { // If it's in the special 0.2-sec reverse-off state
      dotClass += 'off reversed'; // Display as off but with reversed color (red with orange glow for example)
  } else {
      dotClass += (isOn ? 'on' : 'off');
      if (isReversed) {
          dotClass += ' reversed';
      }
  }
  dot.className = dotClass;

  // Update status text
  statusText.innerText = 'Status: ' + (isOn ? 'ON' : 'OFF');

  // Update polarity status text and button text
  polarityStatus.innerText = 'Polarity: ' + (isReversed ? 'Reversed' : 'Normal');
  if (polarityToggleButton) {
      polarityToggleButton.innerText = 'Toggle Polarity'; // Always "Toggle Polarity"
  }

  // Update input field for duration
  // If the electromagnet is fully off and not reversing, reset duration to 0 in UI
  if (!isOn && !isReversingOff) {
      durationInput.value = 0;
  } else {
      durationInput.value = durationMs;
  }


  // Update local data for the electromagnet
  electromagnetsData[index].isOn = isOn;
  electromagnetsData[index].isReversed = isReversed;
  electromagnetsData[index].durationMs = durationMs;
  electromagnetsData[index].isReversingOff = isReversingOff;
}

/**
 * Sends a command to the ESP32 server to update an electromagnet's state.
 * @param {number} index - The index of the electromagnet.
 * @param {string} state - The desired state ('on', 'off', or 'toggle_polarity').
 * @param {boolean} reversed - The desired polarity (true for reversed, false for normal).
 * @param {number} duration - The desired duration in milliseconds (0 for continuous).
 */
function sendCommand(index, state, reversed, duration) {
  const xhr = new XMLHttpRequest();
  const url = `/control?index=${index}&state=${state}&reversed=${reversed ? 'true' : 'false'}&duration=${duration}`;
  xhr.open('GET', url, true);
  xhr.onreadystatechange = function() {
    if (xhr.readyState == 4) {
        if (xhr.status == 200) {
            try {
                const response = JSON.parse(xhr.responseText);
                // console.log('Response for EM ' + index + ':', response); // Removed for cleaner console during playback
                updateUI(response.index, response.isOn, response.isReversed, response.durationMs, response.isReversingOff);
            } catch (e) {
                console.error('Error parsing JSON response:', e, xhr.responseText);
            }
        } else {
            console.error('Error sending command:', xhr.status, xhr.responseText);
        }
    }
  };
  xhr.send();
}

/**
 * Fetches the initial state of all electromagnets from the ESP32 and populates the UI.
 * Also fetches and displays the ESP32's IP address.
 */
async function fetchInitialState() {
    try {
        // Fetch all electromagnet states
        const response = await fetch('/state');
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        const initialStates = await response.json();
        
        // Check if electromagnetsData needs re-initialization (e.g., first load or if number of EMs changed)
        if (electromagnetsData.length === 0 || electromagnetsData.length !== initialStates.length) {
            electromagnetsData = Array(initialStates.length).fill().map((_, i) => ({
                isOn: false,
                isReversed: false,
                durationMs: 0,
                isReversingOff: false
            })); // Initialize with default values
        }
        
        const container = document.getElementById('electromagnet-container');
        // Only clear and recreate if the container is empty, or if the number of EMs changed
        if (container.innerHTML === '' || container.children.length !== electromagnetsData.length) {
            container.innerHTML = ''; // Clear existing content
            // Create UI for each electromagnet
            initialStates.forEach((em, i) => { // Use initialStates length for creation
                const emControlDiv = document.createElement('div');
                emControlDiv.className = 'em-control';
                emControlDiv.innerHTML = `
                    <div class="dot-label">EM ${i + 1}</div> <div id="dot_${i}" class="dot" onclick="toggleElectromagnet(${i})"></div>
                    <p class="status-text" id="status_text_${i}">Status: OFF</p>
                    <p class="status-text" id="polarity_status_${i}">Polarity: Normal</p>
                    <div class="controls">
                        <button id="polarity_toggle_btn_${i}" onclick="togglePolarity(${i})">Toggle Polarity</button>
                        <input type="number" id="duration_${i}" value="0" min="0" placeholder="Duration (ms)" onchange="setDuration(${i})"> ms (0 for continuous)
                    </div>
                `;
                container.appendChild(emControlDiv);
            });
        }
        
        // Always update UI with the fetched initial state, including the new isReversingOff field
        initialStates.forEach((em, i) => {
             updateUI(i, em.isOn, em.isReversed, em.durationMs, em.isReversingOff || false); // Default to false if not present
        });


        // Fetch and display IP address (only once or when needed)
        // This part can be optimized to not fetch on every interval unless IP changes are expected
        if (document.getElementById('ip-address').innerText === 'Loading...' || document.getElementById('ip-address').innerText.startsWith('Error')) {
            const ipResponse = await fetch('/ip');
            const ipAddress = await ipResponse.text();
            document.getElementById('ip-address').innerText = ipAddress;
        }

    } catch (error) {
        console.error('Error fetching initial state:', error);
        document.getElementById('ip-address').innerText = 'Error fetching IP';
    }
}

// --- Pattern Editor Functions ---

/**
 * Records the current state of all electromagnets as a new frame in the pattern.
 */
function recordCurrentState() {
    // Clone the current states of isOn and isReversed for all electromagnets
    const currentFrameEMStates = electromagnetsData.map(em => ({
        isOn: em.isOn,
        isReversed: em.isReversed
    }));
    const defaultDurationInput = document.getElementById('default-new-frame-duration');
    let defaultDuration = parseInt(defaultDurationInput.value);
    if (isNaN(defaultDuration) || defaultDuration < 50) {
        defaultDuration = 2000; // Fallback to default if invalid
        defaultDurationInput.value = defaultDuration;
        console.warn('Invalid default frame duration. Reset to 2000ms.');
    }

    currentPattern.push({ emStates: currentFrameEMStates, duration: defaultDuration });
    updatePatternUI();
    console.log('Recorded new frame. Total frames:', currentPattern.length);
}

/**
 * Clears all frames from the current pattern.
 */
function clearPattern() {
    if (confirm('Are you sure you want to clear the entire pattern?')) {
        stopPattern(); // Stop playback if active
        currentPattern = [];
        currentPatternFrameIndex = 0;
        updatePatternUI();
        console.log('Pattern cleared.');
    }
}

/**
 * Updates the display for the number of frames and the pattern preview.
 */
function updatePatternUI() {
    document.getElementById('frame-count').innerText = currentPattern.length;
    const patternPreview = document.getElementById('pattern-preview');
    patternPreview.innerHTML = ''; // Clear existing preview

    if (currentPattern.length === 0) {
        patternPreview.innerHTML = '<p id="no-frames-message" style="text-align: center; color: #888;">No frames recorded. Click "Record Current State" to begin.</p>';
        return;
    } else {
        const noFramesMessage = document.getElementById('no-frames-message');
        if (noFramesMessage) noFramesMessage.remove(); // Remove message if frames exist
    }


    currentPattern.forEach((frame, frameIndex) => {
        const frameDiv = document.createElement('div');
        frameDiv.className = 'pattern-frame-display';
        frameDiv.setAttribute('data-frame-index', frameIndex); // Store index for easier access
        
        frameDiv.innerHTML = `
            <strong>Frame ${frameIndex + 1}</strong>
            <button class="remove-frame-btn" onclick="event.stopPropagation(); removeFrame(${frameIndex});">X</button>
        `;
        
        const dotsContainer = document.createElement('div');
        dotsContainer.className = 'frame-dots-container';

        frame.emStates.forEach(emState => {
            const miniDot = document.createElement('div');
            miniDot.className = 'mini-dot';
            if (emState.isOn) {
                miniDot.classList.add('on');
                if (emState.isReversed) {
                    miniDot.classList.add('reversed');
                }
            } else {
                miniDot.classList.add('off');
                // For "off" state, the mini-dot can reflect if it would be reversed if turned on
                // This is a stylistic choice for pattern preview.
                if (emState.isReversed) {
                    miniDot.classList.add('reversed');
                }
            }
            dotsContainer.appendChild(miniDot);
        });
        frameDiv.appendChild(dotsContainer);

        // Add duration input for THIS frame
        const durationControls = document.createElement('div');
        durationControls.innerHTML = `
            <label for="frame-duration-${frameIndex}">Dur (ms):</label>
            <input type="number" id="frame-duration-${frameIndex}" value="${frame.duration}" min="50"
                   onchange="updateFrameDuration(${frameIndex}, this.value)">
        `;
        frameDiv.appendChild(durationControls);

        // Add a click listener to apply this frame directly to the electromagnets
        frameDiv.onclick = () => {
            stopPattern(); // Stop playback
            applyPatternFrame(frameIndex); // Apply selected frame
            currentPatternFrameIndex = frameIndex; // Set current index for potential playback restart
            // Highlight manually selected frame
            Array.from(patternPreview.children).forEach((el, idx) => {
                if (idx === frameIndex) {
                    el.classList.add('active-frame');
                } else {
                    el.classList.remove('active-frame');
                }
            });
        };

        patternPreview.appendChild(frameDiv);
    });

    // Highlight the current playing frame if playback is active
    if (patternPlaybackTimeout && currentPattern.length > 0) {
        const activeFrameElement = patternPreview.children[currentPatternFrameIndex];
        if (activeFrameElement) {
            activeFrameElement.classList.add('active-frame');
            // Scroll to the active frame if it's out of view
            activeFrameElement.scrollIntoView({ behavior: 'smooth', inline: 'center' });
        }
    }
}

/**
 * Updates the duration for a specific frame in the pattern.
 * @param {number} frameIndex - The index of the frame to update.
 * @param {string} newDurationStr - The new duration value as a string.
 */
function updateFrameDuration(frameIndex, newDurationStr) {
    let newDuration = parseInt(newDurationStr);
    if (isNaN(newDuration) || newDuration < 50) {
        // Revert input to old valid value if invalid
        document.getElementById(`frame-duration-${frameIndex}`).value = currentPattern[frameIndex].duration;
        console.warn(`Invalid duration for frame ${frameIndex + 1}. Must be at least 50ms.`);
        return;
    }
    currentPattern[frameIndex].duration = newDuration;
    console.log(`Frame ${frameIndex + 1} duration updated to: ${newDuration}ms`);
    // If playing, restart with new duration from the current point
    if (patternPlaybackTimeout) {
        playPattern(); 
    }
}

/**
 * Removes a specific frame from the pattern.
 * @param {number} frameIndex - The index of the frame to remove.
 */
function removeFrame(frameIndex) {
    if (confirm(`Are you sure you want to remove Frame ${frameIndex + 1}?`)) {
        currentPattern.splice(frameIndex, 1);
        stopPattern(); // Stop playback to reset index
        updatePatternUI();
        console.log(`Frame ${frameIndex + 1} removed. Total frames: ${currentPattern.length}`);
    }
}


/**
 * Applies a specific frame from the current pattern to the physical electromagnets.
 * @param {number} frameIndex - The index of the frame to apply.
 */
function applyPatternFrame(frameIndex) {
    if (frameIndex < 0 || frameIndex >= currentPattern.length) {
        console.error('Invalid frame index:', frameIndex);
        stopPattern(); // Stop if somehow an invalid frame is requested
        return;
    }
    const frame = currentPattern[frameIndex];
    frame.emStates.forEach((emState, i) => {
        // Send state to ESP32 without duration, as pattern frames are discrete
        // The ESP32 will handle the 1-sec reverse for 'off' automatically.
        sendCommand(i, emState.isOn ? 'on' : 'off', emState.isReversed, 0); // Duration 0 for pattern frames
    });
    
    // Update active frame highlight
    const patternPreview = document.getElementById('pattern-preview');
    Array.from(patternPreview.children).forEach((el, idx) => {
        if (idx === frameIndex) {
            el.classList.add('active-frame');
            el.scrollIntoView({ behavior: 'smooth', inline: 'center' });
        } else {
            el.classList.remove('active-frame');
        }
    });
}

/**
 * Schedules the next frame in the pattern using setTimeout.
 */
function scheduleNextFrame() {
    if (!patternPlaybackTimeout && !currentPattern.length === 0) {
        // This function is only called if playback is initiated via playPattern()
        // It's a recursive call, so if patternPlaybackTimeout is null, it means stopPattern() was called
        return; 
    }

    const currentFrame = currentPattern[currentPatternFrameIndex];
    const delay = currentFrame.duration; // Use the duration of the current frame

    patternPlaybackTimeout = setTimeout(() => {
        currentPatternFrameIndex++;
        if (currentPatternFrameIndex >= currentPattern.length) {
            currentPatternFrameIndex = 0; // Loop pattern
        }
        applyPatternFrame(currentPatternFrameIndex);
        scheduleNextFrame(); // Schedule the next frame recursively
    }, delay);
}


/**
 * Starts playing the current pattern.
 */
function playPattern() {
    if (currentPattern.length === 0) {
        console.warn('No pattern to play. Record some states first.');
        return;
    }
    stopPattern(); // Stop any existing playback

    // Send a command to ESP32 to start pattern playback
    fetch('/pattern?action=play')
        .then(response => {
            if (!response.ok) throw new Error('Failed to start pattern on ESP32');
            console.log('Pattern playback initiated on ESP32.');
            currentPatternFrameIndex = 0;
            applyPatternFrame(currentPatternFrameIndex); // Apply first frame immediately
            scheduleNextFrame(); // Start scheduling frames
        })
        .catch(error => console.error('Error playing pattern:', error));
}

/**
 * Stops pattern playback.
 */
function stopPattern() {
    if (patternPlaybackTimeout) {
        clearTimeout(patternPlaybackTimeout);
        patternPlaybackTimeout = null;
        // Remove active frame highlight
        const patternPreview = document.getElementById('pattern-preview');
        Array.from(patternPreview.children).forEach(el => el.classList.remove('active-frame'));
        console.log('Pattern playback stopped.');

        // Also tell ESP32 to stop pattern playback and turn off all EMs
        fetch('/pattern?action=stop')
            .then(response => {
                if (!response.ok) throw new Error('Failed to stop pattern on ESP32');
                console.log('Pattern stopped command sent to ESP32.');
            })
            .catch(error => console.error('Error stopping pattern on ESP32:', error));
    }
}


/**
 * Sends the current pattern to the ESP32 to be saved (in memory for now).
 */
async function savePattern() {
    try {
        if (currentPattern.length === 0) {
            console.warn('No pattern to save.');
            return;
        }
        const patternString = JSON.stringify(currentPattern);
        const encodedPattern = encodeURIComponent(patternString);
        // Note: GET request URL length limits may apply for very large patterns.
        const url = `/pattern?action=save&data=${encodedPattern}`;
        const response = await fetch(url);
        if (response.ok) {
            console.log('Pattern saved successfully on ESP32!');
        } else {
            console.error('Failed to save pattern on ESP32:', response.status, await response.text());
        }
    } catch (error) {
        console.error('Error saving pattern:', error);
    }
}

/**
 * Loads a pattern from the ESP32.
 */
async function loadPattern() {
    try {
        const response = await fetch('/pattern?action=load');
        if (response.ok) {
            const data = await response.json();
            if (data && Array.isArray(data)) { // ESP32 now sends just the array of frames
                currentPattern = data;
                updatePatternUI();
                console.log('Pattern loaded successfully from ESP32!');
                stopPattern(); // Ensure pattern is stopped after loading
            } else {
                console.warn('No pattern found or invalid format on ESP32.');
                clearPattern(); // Clear local pattern if none loaded
            }
        } else {
            console.error('Failed to load pattern from ESP32:', response.status, await response.text());
        }
    } catch (error) {
        console.error('Error loading pattern:', error);
    }
}


// Call fetchInitialState when the window loads
window.onload = function() {
    fetchInitialState(); // Initial fetch
    // Set up periodic refresh to keep UI in sync with ESP32 state
    refreshIntervalId = setInterval(fetchInitialState, 1000); // Refresh every 1 seconds

    // Initialize default new frame duration input
    document.getElementById('default-new-frame-duration').value = 2000; // Default to 2000ms
    updatePatternUI(); // Initial draw of pattern editor (will show no frames message)
};

// Optional: Clear interval if the page is unloaded to prevent memory leaks
window.onbeforeunload = function() {
    if (refreshIntervalId) {
        clearInterval(refreshIntervalId);
    }
    stopPattern(); // Stop pattern playback on unload
};
