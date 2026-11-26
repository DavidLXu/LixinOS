//! POSIX identities and mode overrides shared on disk with the native kernel.
use crate::{disk::FileData, process::Volume, user};
use alloc::{string::ToString, vec::Vec};

const META: &str = "/.lixin-permissions";
#[repr(C)]
#[derive(Clone,Copy,Default,PartialEq)]
pub(crate) struct Mode { uid:u32, gid:u32, mode:u32 }
unsafe extern "C" {
    fn id_uid(slot:u32)->u32;
    fn id_gid(slot:u32)->u32;
    fn id_mask(slot:u32)->u32;
    fn id_default(path:*const u8,n:u32,dir:i32,out:*mut Mode);
    fn id_mode(data:*const u8,n:u32,path:*const u8,len:u32,dir:i32,out:*mut Mode)->i32;
    fn id_encode(data:*const u8,n:u32,path:*const u8,len:u32,mode:*const Mode,out:*mut u8,cap:u32)->i64;
    fn id_path(slot:u32,data:*const u8,n:u32,path:*const u8,len:u32,dir:i32,bits:u32)->i32;
    fn id_modify(slot:u32,m:*mut Mode,op:u32,a:u32,b:u32)->i32;
}

fn slot()->u32 { user::bank() as u32 }
pub fn effective_ids()->[u32;2] {unsafe {[id_uid(slot()),id_gid(slot())]}}
pub fn unprivileged()->bool { unsafe { id_uid(slot())!=0 } }
pub(crate) fn records(volume:&Volume)->Result<&[u8], &'static str> {
    match volume.files.get(META) {
        None => Ok(&[]),
        Some(Some(f)) => f.try_as_slice(),
        Some(None) => Err("invalid permission metadata"),
    }
}
fn kind(volume:&Volume,path:&str)->i32 {
    volume.files.get(path).map_or(if volume.is_virtual_proc(path) { (path=="/proc") as i32 } else { -1 },|v|v.is_none() as i32)
}
pub fn allowed(volume:&Volume,path:&str,bits:u32)->bool {
    if !unprivileged() { return true; }
    let Ok(b)=records(volume) else { return false; };
    unsafe { id_path(slot(),b.as_ptr(),b.len() as u32,path.as_ptr(),path.len() as u32,kind(volume,path),bits)!=0 }
}
pub(crate) fn get(volume:&Volume,path:&str)->Option<Mode> {
    let kind=kind(volume,path);
    if kind<0 {return None;}
    let b=records(volume).ok()?;
    let mut m=Mode::default();
    (unsafe { id_mode(b.as_ptr(),b.len() as u32,path.as_ptr(),path.len() as u32,kind,&mut m) }!=0).then_some(m)
}
fn rewrite(volume:&Volume,path:&str,m:Option<&Mode>)->Result<Vec<u8>,&'static str> {
    let old=records(volume)?;
    encode(old,path,m)
}
pub(crate) fn encode(old:&[u8],path:&str,m:Option<&Mode>)->Result<Vec<u8>, &'static str> {
    let cap=old.len()+16+path.len();
    let mut new=Vec::new();
    new.try_reserve_exact(cap).map_err(|_|"out of memory for permissions")?;
    let size=unsafe { id_encode(old.as_ptr(),old.len() as u32,path.as_ptr(),path.len() as u32,
        m.map_or(core::ptr::null(),core::ptr::from_ref),new.as_mut_ptr(),cap as u32) };
    if size<0 {return Err("invalid or full permission metadata");}
    unsafe {new.set_len(size as usize);}
    Ok(new)
}
pub fn syscall(volume:&mut Volume,path:&str,op:u32,words:&mut [u32])->i64 {
    if op==16 || op==17 {
        if kind(volume,path)>=0 {return if words[1]!=0 || op==17 {-17} else {0};}
        if volume.require_writable(path).is_err() || volume.files.len()+2>=crate::disk::ENTRY_LIMIT {return -13;}
        let parent=path.rsplit_once('/').map(|(dir,_)|if dir.is_empty(){"/"}else{dir});
        if parent.is_none_or(|dir|kind(volume,dir)!=1) {return -2;}
        let m=Mode{uid:unsafe{id_uid(slot())},gid:unsafe{id_gid(slot())},mode:words[0]&0o777 & !unsafe{id_mask(slot())}};
        let Ok(data)=rewrite(volume,path,Some(&m)) else {return -28;};
        let old=volume.files.insert(META.to_string(),Some(FileData::from_vec(data)));
        volume.files.insert(path.to_string(),(op==16).then(||FileData::from_vec(Vec::new())));
        if volume.save_changes([(META,old.is_some()),(path,false)].into_iter(),&crate::disk::Times::new()).is_err() {
            volume.files.remove(path);restore(volume,old);return -5;
        }
        return 0;
    }
    if !allowed(volume,path,0) { return -13; }
    let Some(mut m)=get(volume,path) else {return -2;};
    if op==12 {words[..3].copy_from_slice(&[m.uid,m.gid,m.mode]);return 0;}
    if op==15 {return if words[0]&!7==0 && allowed(volume,path,words[0]) {0} else {-13};}
    if path==META || volume.is_virtual_proc(path) || !crate::process::agent_write_allowed(path) {return -13;}
    let res=unsafe {id_modify(slot(),&mut m,op,words[0],words[1])};
    if res!=0 {return res as i64;}
    let Ok(data)=rewrite(volume,path,Some(&m)) else {return -28;};
    let old=volume.files.insert(META.to_string(),Some(FileData::from_vec(data)));
    if volume.save_changes(core::iter::once((META,old.is_some())),&crate::disk::Times::new()).is_err() {
        restore(volume,old);return -5;
    }
    0
}
pub fn restore(volume:&mut Volume,old:Option<Option<FileData>>) {
    match old {Some(v)=>{volume.files.insert(META.to_string(),v);},None=>{volume.files.remove(META);}}
}
pub fn update_entry(volume:&mut Volume,path:&str,existed:bool)->Result<bool,&'static str> {
    let kind=kind(volume,path);
    if path==META || existed==(kind>=0) {return Ok(false);}
    if kind<0 {
        // Remove the previous owner's mode when a name becomes available again.
        let data=records(volume)?;
        let mut m=Mode::default();
        unsafe extern "C" {fn id_find(data:*const u8,size:u32,path:*const u8,len:u32,out:*mut Mode,at:*mut u32)->i64;}
        let found=unsafe {id_find(data.as_ptr(),data.len() as u32,path.as_ptr(),path.len() as u32,&mut m,core::ptr::null_mut())};
        if found<0 {return Err("invalid permissions");}
        if found==0 {return Ok(false);}
    } else {
        if !unprivileged() {return Ok(false);}
        let data=records(volume)?;
        let mut existing=Mode::default();
        unsafe extern "C" {fn id_find(data:*const u8,size:u32,path:*const u8,len:u32,out:*mut Mode,at:*mut u32)->i64;}
        if unsafe {id_find(data.as_ptr(),data.len() as u32,path.as_ptr(),path.len() as u32,&mut existing,core::ptr::null_mut())}==1 {return Ok(false);}
    }
    let mut m=Mode::default();
    unsafe {id_default(path.as_ptr(),path.len() as u32,kind,&mut m);}
    let expected=Mode{uid:unsafe{id_uid(slot())},gid:unsafe{id_gid(slot())},
        mode:(if kind==1{0o777}else{0o666}) & !unsafe{id_mask(slot())}};
    if kind>=0 && expected==m {return Ok(false);}
    let new=rewrite(volume,path,(kind>=0).then_some(&expected))?;
    volume.files.insert(META.to_string(),Some(FileData::from_vec(new)));
    Ok(true)
}
pub const METADATA:&str=META;
