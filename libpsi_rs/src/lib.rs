// Copyright 2025, The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! This module provides functions to initialize, register and unregister
//! monitors, and to parse PSI data.
use anyhow::anyhow;
use anyhow::Context;
use anyhow::Result;
use log::warn;
use nix::sys::epoll::{Epoll, EpollEvent, EpollFlags};
use std::fs::File;
use std::io::Write;
use std::os::fd::BorrowedFd;

/// Represents the type of PSI stall.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PsiStallType {
    /// Some processes are stalled on resources.
    Some,
    /// System is fully stalled on resources.
    Full,
}

impl PsiStallType {
    fn stall_type(&self) -> &str {
        match self {
            PsiStallType::Some => "some",
            PsiStallType::Full => "full",
        }
    }
}

/// Represents the PSI resource being monitored.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PsiResource {
    /// Memory resource
    Memory,
    /// IO resource
    Io,
    /// Cpu resource
    Cpu,
}

impl PsiResource {
    /// Paths to the PSI resource files in `/proc/pressure/`.
    fn resource_file(&self) -> &str {
        match self {
            PsiResource::Memory => "/proc/pressure/memory",
            PsiResource::Io => "/proc/pressure/io",
            PsiResource::Cpu => "/proc/pressure/cpu",
        }
    }
}

/// Structure to hold PSI statistics.
#[derive(Debug, Default, Clone, Copy)]
pub struct PsiStats {
    /// Average stall time over 10 seconds
    pub avg10: f32,
    /// Average stall time over 60 seconds
    pub avg60: f32,
    /// Average stall time over 300 seconds
    pub avg300: f32,
    /// Total stall time
    pub total: u64,
}

/// Initializes a PSI monitor.
///
/// Opens the appropriate PSI resource file, configures the stall type,
/// threshold, and window, and returns the file descriptor.
///
/// # Arguments
/// * `stall_type` - The type of stall to monitor (`PsiStallType::Some` or `PsiStallType::Full`).
/// * `threshold_us` - The threshold in microseconds.
/// * `window_us` - The window in microseconds.
/// * `resource` - The PSI resource to monitor (`PsiResource::Memory`, `PsiResource::Io`,
///   `PsiResource::Cpu`.).
///
/// # Returns
/// A `Result` containing the file on success, or an `Error` on failure.
pub fn init_psi_monitor(
    stall_type: PsiStallType,
    threshold_us: i32,
    window_us: i32,
    resource: PsiResource,
) -> Result<File> {
    let path_str = resource.resource_file();

    let mut file = File::options()
        .read(true)
        .write(true)
        .open(path_str)
        .context("Failed to open PSI monitor file")?;
    let stall_type = stall_type.stall_type();

    let config_str = format!("{} {} {}", stall_type, threshold_us, window_us);
    if !(50_000..=1_000_000).contains(&threshold_us) {
        return Err(anyhow!(
            "Stall threshold out of bounds.
                           Value must be between 50_000us and 1_000_000us. Received: {}us",
            threshold_us
        ));
    }

    if !(500_000..=10_000_000).contains(&window_us) {
        return Err(anyhow!(
            "Window threshold out of bounds.
                           Value must be between 500_000us and 10_000_000us. Received: {}us",
            window_us
        ));
    }

    file.write(config_str.as_bytes())
        .context("failed to write config to PSI monitor")?;

    Ok(file)
}

/// Registers a PSI monitor file descriptor with an epoll instance.
///
/// # Arguments
/// * `epollfd` - The file descriptor of the epoll instance.
/// * `fd` - The file of the PSI monitor to register.
/// * `data` - A raw pointer to user data to be associated with the event.
///
/// # Returns
/// A `Result` indicating success or an `Error` on failure.
pub fn register_psi_monitor(epoll: &Epoll, fd: BorrowedFd, data: u64) -> Result<()> {
    let event = EpollEvent::new(EpollFlags::EPOLLPRI, data);
    epoll
        .add(fd, event)
        .context("failed to register psi monitor")
}

/// Unregisters a PSI monitor file descriptor from an epoll instance.
///
/// # Arguments
/// * `epollfd` - The file descriptor of the epoll instance.
/// * `fd` - The file descriptor of the PSI monitor to unregister.
///
/// # Returns
/// A `Result` indicating success or an `Error` on failure.
pub fn unregister_psi_monitor(epoll: Epoll, fd: File) -> Result<()> {
    epoll.delete(fd).context("failed to delete psi monitor")
}

/// Parses a line from a PSI file to extract stall statistics.
///
/// # Arguments
/// * `line` - The string line read from the PSI file.
/// * `stall_type` - The expected stall type for this line.
/// * `stats` - A mutable array of `PsiStats` to store the parsed data. The parsed statistics will
///   be stored at `stats[stall_type]`.
///
/// # Returns
/// A `Result` indicating success or an `Error` on failure.
pub fn parse_psi_line(line: &str, stall_type: PsiStallType) -> Result<PsiStats> {
    let parts: Vec<&str> = line.split_whitespace().collect();
    if parts.len() < 5 {
        return Err(anyhow!("Invalid PSI line format: not enough parts"));
    }

    let parsed_stall_type_name = parts[0];
    let stall_type = stall_type.stall_type();
    if parsed_stall_type_name != stall_type {
        return Err(anyhow!(
            "Mismatched stall type: expected '{}', got '{}'",
            stall_type,
            parsed_stall_type_name
        ));
    }

    let mut new_stats = PsiStats::default();

    // Skip some|full
    for part in parts.iter().skip(1) {
        if let Some(rest) = part.strip_prefix("avg10=") {
            new_stats.avg10 = rest.parse::<f32>().context("Failed to parse avg10")?;
        } else if let Some(rest) = part.strip_prefix("avg60=") {
            new_stats.avg60 = rest.parse::<f32>().context("Failed to parse avg60")?;
        } else if let Some(rest) = part.strip_prefix("avg300=") {
            new_stats.avg300 = rest.parse::<f32>().context("Failed to parse avg300")?;
        } else if let Some(rest) = part.strip_prefix("total=") {
            new_stats.total = rest.parse::<u64>().context("Failed to parse total")?;
        } else {
            warn!("unrecognized part: {}", part);
        }
    }

    Ok(new_stats)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_psi_line_success() -> Result<()> {
        let line_some = "some avg10=0.01 avg60=0.02 avg300=0.03 total=12345";
        let line_full = "full avg10=1.01 avg60=1.02 avg300=1.03 total=67890";
        let mut stats = [PsiStats::default(), PsiStats::default()];

        parse_psi_line(line_some, PsiStallType::Some, &mut stats)?;
        assert_eq!(stats[0].avg10, 0.01);
        assert_eq!(stats[0].avg60, 0.02);
        assert_eq!(stats[0].avg300, 0.03);
        assert_eq!(stats[0].total, 12345);

        parse_psi_line(line_full, PsiStallType::Full, &mut stats)?;
        assert_eq!(stats[1].avg10, 1.01);
        assert_eq!(stats[1].avg60, 1.02);
        assert_eq!(stats[1].avg300, 1.03);
        assert_eq!(stats[1].total, 67890);

        Ok(())
    }

    #[test]
    fn test_parse_psi_line_invalid_format() {
        let line = "some avg10=0.01 avg60=0.02 total=12345"; // Missing avg300
        let mut stats = [PsiStats::default()];
        let result = parse_psi_line(line, PsiStallType::Some, &mut stats);
        assert!(result.is_err()); // Parsing should still work, but avg300 would be default
        assert_eq!(stats[0].avg300, 0.0); // Check that it's default
    }

    #[test]
    fn test_parse_psi_line_mismatched_stall_type() {
        let line = "full avg10=0.01 avg60=0.02 avg300=0.03 total=12345";
        let mut stats = [PsiStats::default()];
        let result = parse_psi_line(line, PsiStallType::Some, &mut stats);
        assert!(result.is_err());
    }

    #[test]
    fn test_parse_psi_line_empty_line() {
        let line = "";
        let mut stats = [PsiStats::default()];
        let result = parse_psi_line(line, PsiStallType::Some, &mut stats);
        assert!(result.is_err());
    }
}
