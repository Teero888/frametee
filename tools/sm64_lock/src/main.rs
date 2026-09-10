//! Encrypt or decrypt a FrameTee SM64 runtime using a normalized N64 ROM.
//!
//! The container format intentionally matches Wafel's `libsm64_lock` tool so
//! existing Wafel `.locked` libraries can be inspected with this utility.

use std::{env, error::Error, fs, path::{Path, PathBuf}, process};

use pwbox::{sodium::Sodium, ErasedPwBox, Eraser, Suite};

const USAGE: &str = "usage: sm64_lock (--lock | --unlock) --input FILE --output FILE --rom FILE";

enum Mode {
    Lock,
    Unlock,
}

struct Arguments {
    mode: Mode,
    input: PathBuf,
    output: PathBuf,
    rom: PathBuf,
}

fn main() {
    if let Err(error) = run() {
        eprintln!("sm64_lock: {error}");
        process::exit(1);
    }
}

fn run() -> Result<(), Box<dyn Error>> {
    let args = parse_arguments(env::args().skip(1))?;
    let input = fs::read(&args.input)?;
    let rom = normalize_rom(&fs::read(&args.rom)?)?;

    let output = match args.mode {
        Mode::Lock => lock(&input, &rom)?,
        Mode::Unlock => unlock(&input, &rom)?,
    };
    write_atomically(&args.output, &output)?;
    Ok(())
}

fn parse_arguments(arguments: impl Iterator<Item = String>) -> Result<Arguments, Box<dyn Error>> {
    let mut mode = None;
    let mut input = None;
    let mut output = None;
    let mut rom = None;
    let mut arguments = arguments;

    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--lock" => {
                if mode.is_some() {
                    return Err(USAGE.into());
                }
                mode = Some(Mode::Lock);
            }
            "--unlock" => {
                if mode.is_some() {
                    return Err(USAGE.into());
                }
                mode = Some(Mode::Unlock);
            }
            "--input" => input = arguments.next().map(PathBuf::from),
            "--output" => output = arguments.next().map(PathBuf::from),
            "--rom" => rom = arguments.next().map(PathBuf::from),
            "--help" | "-h" => return Err(USAGE.into()),
            _ => return Err(USAGE.into()),
        }
    }

    match (mode, input, output, rom) {
        (Some(mode), Some(input), Some(output), Some(rom)) => Ok(Arguments { mode, input, output, rom }),
        _ => Err(USAGE.into()),
    }
}

fn lock(input: &[u8], rom: &[u8]) -> Result<Vec<u8>, Box<dyn Error>> {
    let password_box = Sodium::build_box(&mut rand::thread_rng()).seal(rom, input)?;
    let mut eraser = Eraser::new();
    eraser.add_suite::<Sodium>();
    let erased = eraser.erase(&password_box)?;
    Ok(serde_json::to_vec(&erased)?)
}

fn unlock(input: &[u8], rom: &[u8]) -> Result<Vec<u8>, Box<dyn Error>> {
    let erased: ErasedPwBox = serde_json::from_slice(input)?;
    let mut eraser = Eraser::new();
    eraser.add_suite::<Sodium>();
    let password_box = eraser.restore(&erased)?;
    Ok(password_box.open(rom)?.to_vec())
}

/// Turn N64 `.n64` and `.v64` byte orders into the canonical `.z64` form.
fn normalize_rom(bytes: &[u8]) -> Result<Vec<u8>, Box<dyn Error>> {
    if bytes.len() < 4 || bytes.len() % 4 != 0 {
        return Err("the ROM has an invalid length".into());
    }
    match &bytes[0..4] {
        b"\x80\x37\x12\x40" => Ok(bytes.to_vec()),
        b"\x37\x80\x40\x12" => Ok(bytes
            .chunks_exact(2)
            .flat_map(|chunk| [chunk[1], chunk[0]])
            .collect()),
        b"\x40\x12\x37\x80" => Ok(bytes
            .chunks_exact(4)
            .flat_map(|chunk| [chunk[3], chunk[2], chunk[1], chunk[0]])
            .collect()),
        _ => Err("the ROM has an unrecognized N64 byte order".into()),
    }
}

fn write_atomically(destination: &Path, bytes: &[u8]) -> Result<(), Box<dyn Error>> {
    let directory = destination.parent().unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(directory)?;
    let temporary = directory.join(format!(
        ".{}.{}.tmp",
        destination.file_name().and_then(|name| name.to_str()).unwrap_or("sm64_lock"),
        process::id()
    ));
    fs::write(&temporary, bytes)?;
    if destination.exists() {
        fs::remove_file(destination)?;
    }
    fs::rename(temporary, destination)?;
    Ok(())
}
