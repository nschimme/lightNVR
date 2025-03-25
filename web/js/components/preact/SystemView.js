/**
 * LightNVR Web Interface SystemView Component
 * Preact component for the system page
 */

import { h } from '../../preact.min.js';
import { html } from '../../html-helper.js';
import { useState, useEffect } from '../../preact.hooks.module.js';
import { showStatusMessage } from './UI.js';

/**
 * Helper function to check if a log level meets the minimum required level
 * This is a JavaScript implementation of the same logic used in the backend
 * 
 * @param {string} logLevel The log level to check
 * @param {string} minLevel The minimum required level
 * @returns {boolean} True if the log level meets the minimum, false otherwise
 */
function log_level_meets_minimum(logLevel, minLevel) {
  // Convert log levels to numeric values for comparison
  let levelValue = 2; // Default to INFO (2)
  let minValue = 2;   // Default to INFO (2)
  
  // Map log level strings to numeric values
  // ERROR = 0, WARNING = 1, INFO = 2, DEBUG = 3
  const logLevelLower = String(logLevel || '').toLowerCase();
  const minLevelLower = String(minLevel || '').toLowerCase();
  
  if (logLevelLower === 'error') {
    levelValue = 0;
  } else if (logLevelLower === 'warning' || logLevelLower === 'warn') {
    levelValue = 1;
  } else if (logLevelLower === 'info') {
    levelValue = 2;
  } else if (logLevelLower === 'debug') {
    levelValue = 3;
  }
  
  if (minLevelLower === 'error') {
    minValue = 0;
  } else if (minLevelLower === 'warning' || minLevelLower === 'warn') {
    minValue = 1;
  } else if (minLevelLower === 'info') {
    minValue = 2;
  } else if (minLevelLower === 'debug') {
    minValue = 3;
  }
  
  // Return true if the log level is less than or equal to the minimum level
  // Lower values are higher priority (ERROR = 0 is highest priority)
  // We want to include all levels with higher or equal priority to the minimum
  return levelValue <= minValue;
}

/**
 * SystemView component
 * @returns {JSX.Element} SystemView component
 */
export function SystemView() {
  const [systemInfo, setSystemInfo] = useState({
    version: '',
    uptime: '',
    cpu: {
      model: '',
      cores: 0,
      usage: 0
    },
    memory: {
      total: 0,
      used: 0,
      free: 0
    },
    systemMemory: {
      total: 0,
      used: 0,
      free: 0
    },
    disk: {
      total: 0,
      used: 0,
      free: 0
    },
    systemDisk: {
      total: 0,
      used: 0,
      free: 0
    },
    network: {
      interfaces: []
    },
    streams: {
      active: 0,
      total: 0
    },
    recordings: {
      count: 0,
      size: 0
    }
  });
  const [logs, setLogs] = useState([]);
  const [logLevel, setLogLevel] = useState('info');
  const [logCount, setLogCount] = useState(100);
  const [isRestarting, setIsRestarting] = useState(false);
  const [isShuttingDown, setIsShuttingDown] = useState(false);
  
  // Load system info and logs on mount
  useEffect(() => {
    loadSystemInfo();
    loadLogs();
    
    // Set up interval to refresh system info
    const interval = setInterval(loadSystemInfo, 10000);
    
    // Initialize WebSocket for system logs if available
    if (window.wsClient && typeof window.wsClient.subscribe === 'function') {
      console.log('Subscribing to system/logs via WebSocket');
      
      // Subscribe to system logs topic with the current log level
      window.wsClient.subscribe('system/logs', { level: logLevel });
      
      // Register handler for system logs updates
      const handleLogsUpdate = (payload) => {
        console.log('Received system logs update via WebSocket:', payload);
        
        if (payload && payload.logs && Array.isArray(payload.logs)) {
          // Clean and normalize logs
          const cleanedLogs = payload.logs.map(log => {
            // Simply use the component attributes directly
            const normalizedLog = {
              timestamp: log.timestamp || 'Unknown',
              level: log.level || 'info',
              message: log.message || ''
            };
            
            // Convert level to lowercase for consistency
            if (normalizedLog.level) {
              normalizedLog.level = normalizedLog.level.toLowerCase();
            }
            
            // Normalize 'warn' to 'warning'
            if (normalizedLog.level === 'warn') {
              normalizedLog.level = 'warning';
            }
            
            return normalizedLog;
          });
          
          // Filter logs based on the selected log level
          const filteredLogs = cleanedLogs.filter(log => {
            // Check if the individual log's level meets the minimum selected level
            return log_level_meets_minimum(log.level, logLevel);
          });
          
          if (filteredLogs.length > 0) {
            console.log(`Adding ${filteredLogs.length} logs that meet the minimum level ${logLevel}`);
            setLogs(prevLogs => {
              // Create a map of existing logs to avoid duplicates
              const existingLogs = new Map();
              prevLogs.forEach(log => {
                // Create a unique key for each log based on timestamp and message
                const key = `${log.timestamp}:${log.message}`;
                existingLogs.set(key, true);
              });
              
              // Filter out logs that already exist
              const newLogs = filteredLogs.filter(log => {
                const key = `${log.timestamp}:${log.message}`;
                return !existingLogs.has(key);
              });
              
              // Return the combined logs
              return [...prevLogs, ...newLogs];
            });
          } else {
            console.log(`No logs meet the minimum level ${logLevel}`);
          }
        }
      };
      
      window.wsClient.on('update', 'system/logs', handleLogsUpdate);
    } else {
      console.log('WebSocket client not available for system logs, using HTTP fallback');
    }
    
    // Clean up interval and WebSocket subscription on unmount
    return () => {
      clearInterval(interval);
      
      // Unsubscribe from system logs topic if WebSocket is available
      if (window.wsClient && typeof window.wsClient.unsubscribe === 'function') {
        console.log('Unsubscribing from system/logs via WebSocket');
        window.wsClient.unsubscribe('system/logs');
        window.wsClient.off('update', 'system/logs');
      }
    };
  }, []);
  
  // Load logs when log level or count changes
  useEffect(() => {
    loadLogs();
    
    // Filter existing logs based on the new log level
    setLogs(prevLogs => {
      return prevLogs.filter(log => log_level_meets_minimum(log.level, logLevel));
    });
    
    // Update WebSocket subscription with new log level if available
    if (window.wsClient && typeof window.wsClient.subscribe === 'function') {
      // Unsubscribe first to avoid duplicate subscriptions
      window.wsClient.unsubscribe('system/logs');
      window.wsClient.off('update', 'system/logs');
      
      // Subscribe with the new log level
      window.wsClient.subscribe('system/logs', { level: logLevel });
      
      // Register handler for system logs updates with the new log level
      const handleLogsUpdate = (payload) => {
        console.log('Received system logs update via WebSocket:', payload);
        
        if (payload && payload.logs && Array.isArray(payload.logs)) {
          // Clean and normalize logs
          const cleanedLogs = payload.logs.map(log => {
            // Simply use the component attributes directly
            const normalizedLog = {
              timestamp: log.timestamp || 'Unknown',
              level: log.level || 'info',
              message: log.message || ''
            };
            
            // Convert level to lowercase for consistency
            if (normalizedLog.level) {
              normalizedLog.level = normalizedLog.level.toLowerCase();
            }
            
            // Normalize 'warn' to 'warning'
            if (normalizedLog.level === 'warn') {
              normalizedLog.level = 'warning';
            }
            
            return normalizedLog;
          });
          
          // Filter logs based on the selected log level
          const filteredLogs = cleanedLogs.filter(log => {
            // Check if the individual log's level meets the minimum selected level
            return log_level_meets_minimum(log.level, logLevel);
          });
          
          if (filteredLogs.length > 0) {
            console.log(`Adding ${filteredLogs.length} logs that meet the minimum level ${logLevel}`);
            setLogs(prevLogs => {
              // Create a map of existing logs to avoid duplicates
              const existingLogs = new Map();
              prevLogs.forEach(log => {
                // Create a unique key for each log based on timestamp and message
                const key = `${log.timestamp}:${log.message}`;
                existingLogs.set(key, true);
              });
              
              // Filter out logs that already exist
              const newLogs = filteredLogs.filter(log => {
                const key = `${log.timestamp}:${log.message}`;
                return !existingLogs.has(key);
              });
              
              // Return the combined logs
              return [...prevLogs, ...newLogs];
            });
          } else {
            console.log(`No logs meet the minimum level ${logLevel}`);
          }
        }
      };
      
      window.wsClient.on('update', 'system/logs', handleLogsUpdate);
      
      console.log(`Updated WebSocket subscription with log level: ${logLevel}`);
    }
  }, [logLevel, logCount]);
  
  // Load system info from API
  const loadSystemInfo = async () => {
    try {
      const response = await fetch('/api/system/info');
      if (!response.ok) {
        throw new Error('Failed to load system info');
      }
      
      const data = await response.json();
      setSystemInfo(data);
    } catch (error) {
      console.error('Error loading system info:', error);
      // Don't show error message for this, just log it
    }
  };
  
  // Load logs from API
  const loadLogs = async () => {
    try {
      const response = await fetch(`/api/system/logs?level=${logLevel}&count=${logCount}`);
      if (!response.ok) {
        throw new Error('Failed to load logs');
      }
      
      const data = await response.json();
      
      // Check if we have logs
      if (data.logs && Array.isArray(data.logs)) {
        // Check if logs are already structured objects or raw strings
        if (data.logs.length > 0 && typeof data.logs[0] === 'object' && data.logs[0].level) {
          // Logs are already structured objects, use them directly
          setLogs(data.logs);
        } else {
          // Logs are raw strings, parse them into structured objects
          const parsedLogs = data.logs.map(logLine => {
            // Parse log line (format: [TIMESTAMP] [LEVEL] MESSAGE)
            let timestamp = 'Unknown';
            let level = 'info';
            let message = logLine;
            
            // Try to extract timestamp and level using regex
            const logRegex = /\[(.*?)\]\s*\[(.*?)\]\s*(.*)/;
            const match = logLine.match(logRegex);
            
            if (match && match.length >= 4) {
              timestamp = match[1];
              level = match[2].toLowerCase();
              message = match[3];
              
              // Normalize log level
              if (level === 'warn') {
                level = 'warning';
              }
            }
            
            return {
              timestamp,
              level,
              message
            };
          });
          
          setLogs(parsedLogs);
        }
      } else {
        setLogs([]);
      }
    } catch (error) {
      console.error('Error loading logs:', error);
      showStatusMessage('Error loading logs: ' + error.message);
    }
  };
  
  // Clear logs
  const clearLogs = async () => {
    if (!confirm('Are you sure you want to clear all logs?')) {
      return;
    }
    
    try {
      showStatusMessage('Clearing logs...');
      
      const response = await fetch('/api/system/logs/clear', {
        method: 'POST'
      });
      
      if (!response.ok) {
        throw new Error('Failed to clear logs');
      }
      
      showStatusMessage('Logs cleared successfully');
      loadLogs(); // Reload logs after clearing
    } catch (error) {
      console.error('Error clearing logs:', error);
      showStatusMessage('Error clearing logs: ' + error.message);
    }
  };
  
  // Restart system
  const restartSystem = async () => {
    if (!confirm('Are you sure you want to restart the system?')) {
      return;
    }
    
    try {
      setIsRestarting(true);
      showStatusMessage('Restarting system...');
      
      const response = await fetch('/api/system/restart', {
        method: 'POST'
      });
      
      if (!response.ok) {
        throw new Error('Failed to restart system');
      }
      
      showStatusMessage('System is restarting. Please wait...');
      
      // Wait for system to restart
      setTimeout(() => {
        window.location.reload();
      }, 10000);
    } catch (error) {
      console.error('Error restarting system:', error);
      showStatusMessage('Error restarting system: ' + error.message);
      setIsRestarting(false);
    }
  };
  
  // Shutdown system
  const shutdownSystem = async () => {
    if (!confirm('Are you sure you want to shut down the system?')) {
      return;
    }
    
    try {
      setIsShuttingDown(true);
      showStatusMessage('Shutting down system...');
      
      const response = await fetch('/api/system/shutdown', {
        method: 'POST'
      });
      
      if (!response.ok) {
        throw new Error('Failed to shut down system');
      }
      
      showStatusMessage('System is shutting down. You will need to manually restart it.');
    } catch (error) {
      console.error('Error shutting down system:', error);
      showStatusMessage('Error shutting down system: ' + error.message);
      setIsShuttingDown(false);
    }
  };
  
  // Format bytes to human-readable size
  const formatBytes = (bytes, decimals = 1) => {
    if (bytes === 0) return '0 Bytes';
    
    const k = 1024;
    const dm = decimals < 0 ? 0 : decimals;
    const sizes = ['Bytes', 'KB', 'MB', 'GB', 'TB', 'PB', 'EB', 'ZB', 'YB'];
    
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    
    return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
  };
  
  // Format uptime
  const formatUptime = (seconds) => {
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const secs = Math.floor(seconds % 60);
    
    let result = '';
    if (days > 0) result += `${days}d `;
    if (hours > 0 || days > 0) result += `${hours}h `;
    if (minutes > 0 || hours > 0 || days > 0) result += `${minutes}m `;
    result += `${secs}s`;
    
    return result;
  };
  
  // Format log level
  const formatLogLevel = (level) => {
    // Handle null or undefined level
    if (level === null || level === undefined) {
      return html`<span class="px-2 py-1 rounded-full text-xs font-medium bg-gray-100 text-gray-800 dark:bg-gray-700 dark:text-gray-300">UNKNOWN</span>`;
    }
    
    // Convert to lowercase string for case-insensitive comparison
    const levelLower = String(level).toLowerCase().trim();
    
    // Match against known log levels
    if (levelLower === 'error' || levelLower === 'err') {
      return html`<span class="px-2 py-1 rounded-full text-xs font-medium bg-red-100 text-red-800 dark:bg-red-900 dark:text-red-200">ERROR</span>`;
    } else if (levelLower === 'warning' || levelLower === 'warn') {
      return html`<span class="px-2 py-1 rounded-full text-xs font-medium bg-yellow-100 text-yellow-800 dark:bg-yellow-900 dark:text-yellow-200">WARN</span>`;
    } else if (levelLower === 'info') {
      return html`<span class="px-2 py-1 rounded-full text-xs font-medium bg-blue-100 text-blue-800 dark:bg-blue-900 dark:text-blue-200">INFO</span>`;
    } else if (levelLower === 'debug' || levelLower === 'dbg') {
      return html`<span class="px-2 py-1 rounded-full text-xs font-medium bg-gray-100 text-gray-800 dark:bg-gray-700 dark:text-gray-300">DEBUG</span>`;
    } else {
      // For any other value, display it as is (uppercase)
      const levelText = String(level).toUpperCase();
      return html`<span class="px-2 py-1 rounded-full text-xs font-medium bg-gray-100 text-gray-800 dark:bg-gray-700 dark:text-gray-300">${levelText}</span>`;
    }
  };
  
  return html`
    <section id="system-page" class="page">
      <div class="page-header flex justify-between items-center mb-4 p-4 bg-white dark:bg-gray-800 rounded-lg shadow">
        <h2 class="text-xl font-bold">System</h2>
        <div class="controls space-x-2">
          <button 
            id="restart-btn" 
            class="px-4 py-2 bg-yellow-600 text-white rounded hover:bg-yellow-700 transition-colors focus:outline-none focus:ring-2 focus:ring-yellow-500 focus:ring-offset-2 dark:focus:ring-offset-gray-800 disabled:opacity-50 disabled:cursor-not-allowed"
            onClick=${restartSystem}
            disabled=${isRestarting || isShuttingDown}
          >
            Restart
          </button>
          <button 
            id="shutdown-btn" 
            class="px-4 py-2 bg-red-600 text-white rounded hover:bg-red-700 transition-colors focus:outline-none focus:ring-2 focus:ring-red-500 focus:ring-offset-2 dark:focus:ring-offset-gray-800 disabled:opacity-50 disabled:cursor-not-allowed"
            onClick=${shutdownSystem}
            disabled=${isRestarting || isShuttingDown}
          >
            Shutdown
          </button>
        </div>
      </div>
      
      <div class="grid grid-cols-1 md:grid-cols-2 gap-4 mb-4">
        <div class="bg-white dark:bg-gray-800 rounded-lg shadow p-4">
          <h3 class="text-lg font-semibold mb-4 pb-2 border-b border-gray-200 dark:border-gray-700">System Information</h3>
          <div class="space-y-2">
            <div class="flex justify-between">
              <span class="font-medium">Version:</span>
              <span>${systemInfo.version || 'Unknown'}</span>
            </div>
            <div class="flex justify-between">
              <span class="font-medium">Uptime:</span>
              <span>${systemInfo.uptime ? formatUptime(systemInfo.uptime) : 'Unknown'}</span>
            </div>
            <div class="flex justify-between">
              <span class="font-medium">CPU Model:</span>
              <span>${systemInfo.cpu?.model || 'Unknown'}</span>
            </div>
            <div class="flex justify-between">
              <span class="font-medium">CPU Cores:</span>
              <span>${systemInfo.cpu?.cores || 'Unknown'}</span>
            </div>
            <div class="flex justify-between items-center">
              <span class="font-medium">CPU Usage:</span>
              <div class="w-32 bg-gray-200 rounded-full h-2.5 dark:bg-gray-700">
                <div class="bg-blue-600 h-2.5 rounded-full" style=${`width: ${systemInfo.cpu?.usage || 0}%`}></div>
              </div>
              <span>${systemInfo.cpu?.usage ? `${systemInfo.cpu.usage.toFixed(1)}%` : 'Unknown'}</span>
            </div>
          </div>
        </div>
        
        <div class="bg-white dark:bg-gray-800 rounded-lg shadow p-4">
          <h3 class="text-lg font-semibold mb-4 pb-2 border-b border-gray-200 dark:border-gray-700">Memory & Storage</h3>
          <div class="space-y-4">
            <div>
              <div class="flex justify-between mb-1">
                <span class="font-medium">LightNVR Memory:</span>
                <span>${systemInfo.memory?.used ? formatBytes(systemInfo.memory.used) : '0'} / ${systemInfo.memory?.total ? formatBytes(systemInfo.memory.total) : '0'}</span>
              </div>
              <div class="w-full bg-gray-200 rounded-full h-2.5 dark:bg-gray-700">
                <div class="bg-blue-600 h-2.5 rounded-full" style=${`width: ${systemInfo.memory?.total ? (systemInfo.memory.used / systemInfo.memory.total * 100).toFixed(1) : 0}%`}></div>
              </div>
            </div>
            <div>
              <div class="flex justify-between mb-1">
                <span class="font-medium">System Memory:</span>
                <span>${systemInfo.systemMemory?.used ? formatBytes(systemInfo.systemMemory.used) : '0'} / ${systemInfo.systemMemory?.total ? formatBytes(systemInfo.systemMemory.total) : '0'}</span>
              </div>
              <div class="w-full bg-gray-200 rounded-full h-2.5 dark:bg-gray-700">
                <div class="bg-blue-600 h-2.5 rounded-full" style=${`width: ${systemInfo.systemMemory?.total ? (systemInfo.systemMemory.used / systemInfo.systemMemory.total * 100).toFixed(1) : 0}%`}></div>
              </div>
            </div>
            <div>
              <div class="flex justify-between mb-1">
                <span class="font-medium">LightNVR Storage:</span>
                <span>${systemInfo.disk?.used ? formatBytes(systemInfo.disk.used) : '0'} / ${systemInfo.disk?.total ? formatBytes(systemInfo.disk.total) : '0'}</span>
              </div>
              <div class="w-full bg-gray-200 rounded-full h-2.5 dark:bg-gray-700">
                <div class="bg-blue-600 h-2.5 rounded-full" style=${`width: ${systemInfo.disk?.total ? (systemInfo.disk.used / systemInfo.disk.total * 100).toFixed(1) : 0}%`}></div>
              </div>
            </div>
            <div>
              <div class="flex justify-between mb-1">
                <span class="font-medium">System Storage:</span>
                <span>${systemInfo.systemDisk?.used ? formatBytes(systemInfo.systemDisk.used) : '0'} / ${systemInfo.systemDisk?.total ? formatBytes(systemInfo.systemDisk.total) : '0'}</span>
              </div>
              <div class="w-full bg-gray-200 rounded-full h-2.5 dark:bg-gray-700">
                <div class="bg-blue-600 h-2.5 rounded-full" style=${`width: ${systemInfo.systemDisk?.total ? (systemInfo.systemDisk.used / systemInfo.systemDisk.total * 100).toFixed(1) : 0}%`}></div>
              </div>
            </div>
          </div>
        </div>
      </div>
      
      <div class="grid grid-cols-1 md:grid-cols-2 gap-4 mb-4">
        <div class="bg-white dark:bg-gray-800 rounded-lg shadow p-4">
          <h3 class="text-lg font-semibold mb-4 pb-2 border-b border-gray-200 dark:border-gray-700">Network Interfaces</h3>
          <div class="space-y-2">
            ${systemInfo.network?.interfaces?.length ? systemInfo.network.interfaces.map(iface => html`
              <div key=${iface.name} class="mb-2 pb-2 border-b border-gray-100 dark:border-gray-700 last:border-0">
                <div class="flex justify-between">
                  <span class="font-medium">${iface.name}:</span>
                  <span>${iface.address || 'No IP'}</span>
                </div>
                <div class="text-sm text-gray-500 dark:text-gray-400">
                  MAC: ${iface.mac || 'Unknown'} | ${iface.up ? 'Up' : 'Down'}
                </div>
              </div>
            `) : html`<div class="text-gray-500 dark:text-gray-400">No network interfaces found</div>`}
          </div>
        </div>
        
        <div class="bg-white dark:bg-gray-800 rounded-lg shadow p-4">
          <h3 class="text-lg font-semibold mb-4 pb-2 border-b border-gray-200 dark:border-gray-700">Streams & Recordings</h3>
          <div class="space-y-2">
            <div class="flex justify-between">
              <span class="font-medium">Active Streams:</span>
              <span>${systemInfo.streams?.active || 0} / ${systemInfo.streams?.total || 0}</span>
            </div>
            <div class="flex justify-between">
              <span class="font-medium">Recordings:</span>
              <span>${systemInfo.recordings?.count || 0}</span>
            </div>
            <div class="flex justify-between">
              <span class="font-medium">Recordings Size:</span>
              <span>${systemInfo.recordings?.size ? formatBytes(systemInfo.recordings.size) : '0'}</span>
            </div>
          </div>
        </div>
      </div>
      
      <div class="bg-white dark:bg-gray-800 rounded-lg shadow p-4 mb-4">
        <div class="flex justify-between items-center mb-4 pb-2 border-b border-gray-200 dark:border-gray-700">
          <h3 class="text-lg font-semibold">System Logs</h3>
          <div class="flex space-x-2">
            <select 
              id="log-level" 
              class="px-3 py-2 border border-gray-300 rounded-md shadow-sm focus:outline-none focus:ring-blue-500 focus:border-blue-500 dark:bg-gray-700 dark:border-gray-600 dark:text-white"
              value=${logLevel}
              onChange=${e => setLogLevel(e.target.value)}
            >
              <option value="error">Error</option>
              <option value="warning">Warning</option>
              <option value="info">Info</option>
              <option value="debug">Debug</option>
            </select>
            <select 
              id="log-count" 
              class="px-3 py-2 border border-gray-300 rounded-md shadow-sm focus:outline-none focus:ring-blue-500 focus:border-blue-500 dark:bg-gray-700 dark:border-gray-600 dark:text-white"
              value=${logCount}
              onChange=${e => setLogCount(parseInt(e.target.value, 10))}
            >
              <option value="50">50 lines</option>
              <option value="100">100 lines</option>
              <option value="200">200 lines</option>
              <option value="500">500 lines</option>
            </select>
            <button 
              id="refresh-logs-btn" 
              class="px-3 py-2 bg-blue-600 text-white rounded hover:bg-blue-700 transition-colors focus:outline-none focus:ring-2 focus:ring-blue-500 focus:ring-offset-2 dark:focus:ring-offset-gray-800"
              onClick=${loadLogs}
            >
              Refresh
            </button>
            <button 
              id="clear-logs-btn" 
              class="px-3 py-2 bg-red-600 text-white rounded hover:bg-red-700 transition-colors focus:outline-none focus:ring-2 focus:ring-red-500 focus:ring-offset-2 dark:focus:ring-offset-gray-800"
              onClick=${clearLogs}
            >
              Clear Logs
            </button>
          </div>
        </div>
        <div class="logs-container bg-gray-100 dark:bg-gray-900 rounded p-4 overflow-auto max-h-96 font-mono text-sm">
          ${logs.length === 0 ? html`
            <div class="text-gray-500 dark:text-gray-400">No logs found</div>
          ` : logs.map((log, index) => html`
            <div key=${index} class="log-entry mb-1 last:mb-0">
              <span class="text-gray-500 dark:text-gray-400">${log.timestamp}</span>
              <span class="mx-2">${formatLogLevel(log.level)}</span>
              <span class=${`log-message ${log.level === 'error' ? 'text-red-600 dark:text-red-400' : ''}`}>${log.message}</span>
            </div>
          `)}
        </div>
      </div>
    </section>
  `;
}

/**
 * Load SystemView component
 */
export function loadSystemView() {
  const mainContent = document.getElementById('main-content');
  if (!mainContent) return;
  
  // Render the SystemView component to the container
  import('../../preact.min.js').then(({ render }) => {
    render(html`<${SystemView} />`, mainContent);
    
    // Refresh system info immediately after rendering
    setTimeout(() => {
      const event = new CustomEvent('refresh-system-info');
      window.dispatchEvent(event);
    }, 100);
  });
}

// Add a global event listener for refreshing system info
window.addEventListener('load', () => {
  window.addEventListener('refresh-system-info', async () => {
    try {
      const response = await fetch('/api/system/info');
      if (response.ok) {
        console.log('System info refreshed');
      }
    } catch (error) {
      console.error('Error refreshing system info:', error);
    }
  });
});
