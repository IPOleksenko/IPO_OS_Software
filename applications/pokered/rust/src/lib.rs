use pokered_app::PokemonGame;
use pokered_core::data::wild_data::GameVersion;
use pokered_renderer::input::InputState;
use pokered_renderer::{FrameBuffer, RenderConfig, Rgba};

pub struct PokeredInstance {
    game: PokemonGame,
    input: InputState,
    frame: FrameBuffer,
}

#[no_mangle]
pub extern "C" fn pokered_ipo_create() -> *mut PokeredInstance {
    let Ok(game) = PokemonGame::new_mobile(GameVersion::Red, None) else {
        return core::ptr::null_mut();
    };
    pokered_core::battle::prepare_battle_rules();
    let frame = FrameBuffer::new(RenderConfig::new(160, 144), Rgba::WHITE);
    let inst = Box::new(PokeredInstance {
        game,
        input: InputState::new(),
        frame,
    });
    Box::into_raw(inst)
}

#[no_mangle]
pub extern "C" fn pokered_ipo_destroy(ptr: *mut PokeredInstance) {
    if !ptr.is_null() {
        unsafe { drop(Box::from_raw(ptr)) };
    }
}

#[no_mangle]
pub extern "C" fn pokered_ipo_tick(ptr: *mut PokeredInstance, input_bits: u8, out_rgba: *mut u8) {
    if ptr.is_null() || out_rgba.is_null() {
        return;
    }
    let inst = unsafe { &mut *ptr };
    inst.input.set_from_bitmask(input_bits);
    inst.game.update(&inst.input);
    inst.input.begin_frame();
    inst.game.draw(&mut inst.frame);
    let buf = unsafe { core::slice::from_raw_parts_mut(out_rgba, 160 * 144 * 4) };
    inst.frame.to_rgba(buf);
}

#[no_mangle]
pub extern "C" fn pokered_ipo_render_audio(ptr: *mut PokeredInstance, out_f32: *mut f32, len: usize) {
    if ptr.is_null() || out_f32.is_null() || len == 0 {
        return;
    }
    let inst = unsafe { &mut *ptr };
    let buf = unsafe { core::slice::from_raw_parts_mut(out_f32, len) };
    if let Some(audio) = &inst.game.audio {
        audio.render_pcm(buf);
    } else {
        buf.fill(0.0);
    }
}
