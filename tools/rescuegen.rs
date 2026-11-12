//! Offline (scratch, not part of M-Flasher): an FM-1 or SMK-37 rescue image from a
//! firmware package and an m-bootloader build, with M-Flasher's own rules:
//! the package's flash.bin laid over erased flash (as `firmware
//! --with-bootloader` would), then the bootloader installed into the boot
//! area (as `bootloader` would, for m-bootloader 1.4 and newer).
//! Usage: rescue-image PKG.fwsc UBOOT.boot OUTDIR [BACKUP.bin]
use crate::boards::{Analysis, CHIP_KEY, FLASH_SIZE};
use crate::formats::*;
use sha2::{Digest, Sha256};

fn describe(name: &str, img: &[u8], flash_id: u32) {
    let a = Analysis::new(img, Some(flash_id), CHIP_KEY);
    println!("{name}: model {}, bootloader {}, firmware {} ({})", a.model_text(), a.bootloader_text,
             a.firmware_text, a.app_status.text());
}

fn hex(d: &[u8]) -> String {
    Sha256::digest(d).iter().map(|b| format!("{b:02x}")).collect()
}

pub fn run(args: &[String]) -> i32 {
    let go = || -> Result<(), String> {
        let [pkg_path, boot_path, out, rest @ ..] = args else { return Err("usage: rescue-image PKG.fwsc UBOOT.boot OUTDIR [BACKUP.bin]".into()) };
        let raw = std::fs::read(pkg_path).map_err(|e| e.to_string())?;
        let boot = std::fs::read(boot_path).map_err(|e| e.to_string())?;
        let pkg = Package::parse(&raw).map_err(|e| e.0)?;
        println!("package {} (model {}), flash.bin {} bytes", pkg.identity, pkg.model(), pkg.flash.len());
        // the flash ID the unit's chip reports, for M-Flasher's model analysis
        let flash_id = if pkg.model() == "FM-1" { 0x856014 } else { 0xEB6014 };
        if let Err(e) = check_boot_area(&pkg.flash) {
            println!("note: the package's own boot area can't start a unit ({}); only its header, file table and chip-key record are used", e.0);
        }
        if reads_config(&boot) { return Err("this bootloader reads the unit's configuration: not m-bootloader >= 1.4".into()); }
        println!("bootloader {:?}", bootloader_identity(&boot));

        let mut img = vec![0xFFu8; FLASH_SIZE];
        img[..pkg.flash.len()].copy_from_slice(&pkg.flash);
        // the boot area's header, file table and chip-key record: from an
        // official package (RESCUE_BOOT_BASE) whose boot area starts a unit
        let base = match std::env::var("RESCUE_BOOT_BASE") {
            Ok(path) => {
                let b = Package::parse(&std::fs::read(&path).map_err(|e| e.to_string())?).map_err(|e| e.0)?;
                check_boot_area(&b.flash).map_err(|e| format!("{}: {}", b.identity, e.0))?;
                println!("boot area base: {} (its boot area starts a unit)", b.identity);
                b.flash[..BOOT_AREA].to_vec()
            }
            Err(_) => img[..BOOT_AREA].to_vec(),
        };
        let area = build_short_boot_area(&base, &boot).map_err(|e| e.0)?;
        println!("chip-key record: {}", area[CONFIG_AT..CONFIG_AT + CONFIG_KEY].iter().map(|b| format!("{b:02x}")).collect::<String>());
        img[..BOOT_AREA].copy_from_slice(&area);
        if app_files_status(&img, CHIP_KEY) != AppStatus::Ok { return Err("the app does not check out under key 980F".into()); }
        describe("rescue image", &img, flash_id);

        std::fs::create_dir_all(out).map_err(|e| e.to_string())?;
        let end = pkg.flash.len();
        for (name, data) in [("image.bin", &img[..]), ("boot.bin", &img[..BOOT_AREA]), ("app.bin", &img[BOOT_AREA..end])] {
            std::fs::write(format!("{out}/{name}"), data).map_err(|e| e.to_string())?;
            println!("{name:10} {:7} bytes  crc32 {:08x}  sha256 {}", data.len(), crc32(data), hex(data));
        }
        println!("app area 0x{BOOT_AREA:05X}..0x{end:05X}");
        let regions: Vec<String> = app_table(&img, CHIP_KEY).iter().map(|e| format!("{} 0x{:05X}+0x{:X} attr {:02X}", e.name, e.addr, e.len, e.attr)).collect();
        println!("app file table: {}", regions.join("; "));
        if let [backup, ..] = rest {
            let b = std::fs::read(backup).map_err(|e| e.to_string())?;
            describe("backup", &b, flash_id);
            match build_short_boot_area(&b, &boot) {
                Ok(from_unit) => {
                    let n = from_unit.iter().zip(&img[..BOOT_AREA]).filter(|(x, y)| x != y).count();
                    println!("boot area built from the backup instead: {n} bytes differ");
                    for (i, (x, y)) in from_unit.iter().zip(&img[..BOOT_AREA]).enumerate().filter(|(_, (x, y))| x != y).take(16) {
                        println!("  0x{i:04X}: backup {x:02X} package {y:02X}");
                    }
                }
                Err(e) => println!("boot area from the backup: {}", e.0),
            }
            let differ = (0..FLASH_SIZE / SECTOR).filter(|s| b[s * SECTOR..(s + 1) * SECTOR] != img[s * SECTOR..(s + 1) * SECTOR]).count();
            println!("sectors that differ from the backup: {differ}");
        }
        Ok(())
    };
    match go() {
        Ok(()) => 0,
        Err(e) => { eprintln!("error: {e}"); 1 }
    }
}
