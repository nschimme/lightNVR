/**
 * LightNVR Timeline Page Component
 * Main component for the timeline view
 */

import { h } from '../../../preact.min.js';
import { html } from '../../../html-helper.js';
import { useState, useEffect, useRef } from '../../../preact.hooks.module.js';
import { TimelineControls } from './TimelineControls.js';
import { TimelineRuler } from './TimelineRuler.js';
import { TimelineSegments } from './TimelineSegments.js';
import { TimelineCursor } from './TimelineCursor.js';
import { TimelinePlayer } from './TimelinePlayer.js';
import { SpeedControls } from './SpeedControls.js';
import { showStatusMessage } from '../UI.js';

// Timeline state store
const timelineState = {
  streams: [],
  timelineSegments: [],
  selectedStream: null,
  selectedDate: null,
  isPlaying: false,
  currentSegmentIndex: -1,
  zoomLevel: 1, // 1 = 1 hour, 2 = 30 minutes, 4 = 15 minutes
  timelineStartHour: 0,
  timelineEndHour: 24,
  currentTime: null,
  playbackSpeed: 1.0,
  listeners: new Set(),

  // Update state and notify listeners
  setState(newState) {
    Object.assign(this, newState);
    this.notifyListeners();
  },

  // Subscribe to state changes
  subscribe(listener) {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  },

  // Notify all listeners of state change
  notifyListeners() {
    this.listeners.forEach(listener => listener(this));
  }
};

/**
 * Format date for input element
 * @param {Date} date - Date to format
 * @returns {string} Formatted date string (YYYY-MM-DD)
 */
function formatDateForInput(date) {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const day = String(date.getDate()).padStart(2, '0');
  return `${year}-${month}-${day}`;
}

/**
 * TimelinePage component
 * @returns {JSX.Element} TimelinePage component
 */
export function TimelinePage() {
  // Local state
  const [state, setState] = useState({
    streams: [],
    selectedStream: '',
    selectedDate: formatDateForInput(new Date()),
    isLoading: false,
    hasData: false
  });

  // Refs
  const timelineContainerRef = useRef(null);

  // Load streams on mount
  useEffect(() => {
    loadStreams();
  }, []);

  // Load streams for dropdown
  const loadStreams = () => {
    setState(prev => ({ ...prev, isLoading: true }));
    
    fetch('/api/streams')
      .then(response => {
        if (!response.ok) {
          throw new Error('Failed to load streams');
        }
        return response.json();
      })
      .then(data => {
        const streams = data || [];
        setState(prev => ({ 
          ...prev, 
          streams,
          isLoading: false
        }));
        
        // Update global state
        timelineState.setState({ streams });
      })
      .catch(error => {
        console.error('Error loading streams:', error);
        showStatusMessage('Error loading streams: ' + error.message, 'error');
        setState(prev => ({ ...prev, isLoading: false }));
      });
  };

  // Load timeline data based on selected stream and date
  const loadTimelineData = () => {
    if (state.selectedStream === '') {
      showStatusMessage('Please select a stream', 'error');
      return;
    }

    setState(prev => ({ ...prev, isLoading: true }));
    showStatusMessage('Loading timeline data...', 'info');
    
    // Calculate start and end times (full day)
    const startDate = new Date(state.selectedDate);
    startDate.setHours(0, 0, 0, 0);
    
    const endDate = new Date(state.selectedDate);
    endDate.setHours(23, 59, 59, 999);
    
    // Format dates for API
    const startTime = startDate.toISOString();
    const endTime = endDate.toISOString();
    
    console.log(`Loading timeline data for stream ${state.selectedStream} from ${startTime} to ${endTime}`);
    
    // Update global state
    timelineState.setState({
      selectedStream: state.selectedStream,
      selectedDate: state.selectedDate
    });
    
    // Fetch timeline segments
    fetch(`/api/timeline/segments?stream=${encodeURIComponent(state.selectedStream)}&start=${encodeURIComponent(startTime)}&end=${encodeURIComponent(endTime)}`)
      .then(response => {
        if (!response.ok) {
          throw new Error('Failed to load timeline data');
        }
        return response.json();
      })
      .then(data => {
        console.log('Timeline data received:', data);
        const timelineSegments = data.segments || [];
        
        if (timelineSegments.length === 0) {
          showStatusMessage('No recordings found for the selected date', 'warning');
          setState(prev => ({ 
            ...prev, 
            isLoading: false,
            hasData: false
          }));
          
          // Update global state
          timelineState.setState({
            timelineSegments: [],
            currentSegmentIndex: -1,
            currentTime: null,
            isPlaying: false
          });
          return;
        }
        
        setState(prev => ({ 
          ...prev, 
          isLoading: false,
          hasData: true
        }));
        
        // Update global state
        timelineState.setState({
          timelineSegments,
          currentSegmentIndex: -1,
          currentTime: null,
          isPlaying: false
        });
        
        // Show success message
        showStatusMessage(`Loaded ${timelineSegments.length} recording segments`, 'success');
      })
      .catch(error => {
        console.error('Error loading timeline data:', error);
        showStatusMessage('Error loading timeline data: ' + error.message, 'error');
        setState(prev => ({ ...prev, isLoading: false, hasData: false }));
      });
  };

  // Handle stream selection change
  const handleStreamChange = (e) => {
    setState(prev => ({ ...prev, selectedStream: e.target.value }));
  };

  // Handle date selection change
  const handleDateChange = (e) => {
    setState(prev => ({ ...prev, selectedDate: e.target.value }));
  };

  return html`
    <div class="timeline-page">
      <div class="flex items-center mb-4">
        <h1 class="text-2xl font-bold">Timeline Playback</h1>
        <div class="ml-4 flex">
          <a href="recordings.html" class="px-3 py-1 bg-gray-300 text-gray-700 dark:bg-gray-700 dark:text-gray-300 hover:bg-gray-400 dark:hover:bg-gray-600 rounded-l-md">Table View</a>
          <a href="timeline.html" class="px-3 py-1 bg-blue-500 text-white rounded-r-md">Timeline View</a>
        </div>
      </div>
      
      <!-- Stream selector and date picker -->
      <div class="flex flex-wrap gap-4 mb-6">
        <div class="stream-selector flex-grow">
          <label for="stream-selector" class="block mb-2">Stream</label>
          <select 
            id="stream-selector" 
            class="w-full p-2 border rounded dark:bg-gray-700 dark:border-gray-600 dark:text-white"
            value=${state.selectedStream}
            onChange=${handleStreamChange}
            disabled=${state.isLoading}
          >
            <option value="" disabled selected>Select a stream</option>
            ${state.streams.map(stream => html`
              <option value=${stream.name}>${stream.name}</option>
            `)}
          </select>
        </div>
        
        <div class="date-selector flex-grow">
          <label for="timeline-date" class="block mb-2">Date</label>
          <input 
            type="date" 
            id="timeline-date" 
            class="w-full p-2 border rounded dark:bg-gray-700 dark:border-gray-600 dark:text-white"
            value=${state.selectedDate}
            onChange=${handleDateChange}
            disabled=${state.isLoading}
          />
        </div>
        
        <div class="flex items-end">
          <button 
            id="apply-button" 
            class="bg-blue-500 hover:bg-blue-600 text-white px-4 py-2 rounded disabled:opacity-50 disabled:cursor-not-allowed"
            onClick=${loadTimelineData}
            disabled=${state.isLoading || state.selectedStream === ''}
          >
            ${state.isLoading ? 'Loading...' : 'Apply'}
          </button>
        </div>
      </div>
      
      <!-- Video player -->
      <${TimelinePlayer} />
      
      <!-- Playback controls -->
      <${TimelineControls} />
      
      <!-- Current time display -->
      <div class="flex justify-between items-center mb-4">
        <div id="time-display" class="timeline-time-display bg-gray-200 dark:bg-gray-700 px-4 py-2 rounded font-mono text-lg">00:00:00</div>
      </div>
      
      <!-- Timeline -->
      <div 
        id="timeline-container" 
        class="relative w-full h-24 bg-gray-200 dark:bg-gray-700 border border-gray-300 dark:border-gray-600 rounded-lg mb-6 overflow-hidden"
        ref=${timelineContainerRef}
      >
        <${TimelineRuler} />
        <${TimelineSegments} />
        <${TimelineCursor} />
      </div>
      
      <!-- Instructions -->
      <div class="mt-6 p-4 bg-gray-200 dark:bg-gray-800 rounded">
        <h3 class="text-lg font-semibold mb-2">How to use the timeline:</h3>
        <ul class="list-disc pl-5">
          <li>Select a stream and date, then click "Apply" to load recordings</li>
          <li>Click on the timeline to seek to a specific time</li>
          <li>Click on a segment (blue bar) to play that recording</li>
          <li>Use the play/pause button to control playback</li>
          <li>Use the zoom buttons to adjust the timeline scale</li>
        </ul>
      </div>
    </div>
  `;
}

// Export the timeline state for use in other components
export { timelineState };
