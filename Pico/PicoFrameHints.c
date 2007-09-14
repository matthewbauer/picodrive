#define CYCLES_M68K_LINE     488 // suitable for both PAL/NTSC
#define CYCLES_M68K_VINT_LAG  68
#define CYCLES_M68K_ASD      148
#define CYCLES_Z80_LINE      228
#define CYCLES_Z80_ASD        69

// pad delay (for 6 button pads)
#define PAD_DELAY \
  if (PicoOpt&0x20) { \
    if(Pico.m.padDelay[0]++ > 25) Pico.m.padTHPhase[0]=0; \
    if(Pico.m.padDelay[1]++ > 25) Pico.m.padTHPhase[1]=0; \
  }

#define Z80_RUN(z80_cycles) \
{ \
  if ((PicoOpt&4) && Pico.m.z80Run) \
  { \
    if (Pico.m.z80Run & 2) z80CycleAim += z80_cycles; \
    else { \
      int cnt = SekCyclesDone() - z80startCycle; \
      cnt = (cnt>>1)-(cnt>>5); \
      if (cnt > (z80_cycles)) cnt = z80_cycles; \
      Pico.m.z80Run |= 2; \
      z80CycleAim+=cnt; \
    } \
    total_z80+=z80_run(z80CycleAim-total_z80); \
  } \
}

// Accurate but slower frame which does hints
static int PicoFrameHints(void)
{
  struct PicoVideo *pv=&Pico.video;
  int total_z80=0,lines,y,lines_vis = 224,z80CycleAim = 0,line_sample;
  int skip=PicoSkipFrame || (PicoOpt&0x10);
  int hint; // Hint counter

  if (Pico.m.pal) {
    //cycles_68k = (int) ((double) OSC_PAL  /  7 / 50 / 312 + 0.4); // should compile to a constant (488)
    //cycles_z80 = (int) ((double) OSC_PAL  / 15 / 50 / 312 + 0.4); // 228
    line_sample = 68;
    if(pv->reg[1]&8) lines_vis = 240;
  } else {
    //cycles_68k = (int) ((double) OSC_NTSC /  7 / 60 / 262 + 0.4); // 488
    //cycles_z80 = (int) ((double) OSC_NTSC / 15 / 60 / 262 + 0.4); // 228
    line_sample = 93;
  }

  SekCyclesReset();

  pv->status&=~0x88; // clear V-Int, come out of vblank

  hint=pv->reg[10]; // Load H-Int counter
  //dprintf("-hint: %i", hint);

  // This is to make active scan longer (needed for Double Dragon 2, mainly)
  SekRun(CYCLES_M68K_ASD);
  Z80_RUN(CYCLES_Z80_ASD);

  for (y=0;y<lines_vis;y++)
  {
    Pico.m.scanline=(short)y;

    // VDP FIFO
    pv->lwrite_cnt -= 12;
    if (pv->lwrite_cnt <= 0) {
      pv->lwrite_cnt=0;
      Pico.video.status|=0x200;
    }

    PAD_DELAY

    // H-Interrupts:
    if (--hint < 0) // y <= lines_vis: Comix Zone, Golden Axe
    {
      hint=pv->reg[10]; // Reload H-Int counter
      pv->pending_ints|=0x10;
      if (pv->reg[0]&0x10) {
        elprintf(EL_INTS, "hint: @ %06x [%i]", SekPc, SekCycleCnt);
        SekInterrupt(4);
      }
    }

    // decide if we draw this line
#if CAN_HANDLE_240_LINES
    if(!skip && ((!(pv->reg[1]&8) && y<224) || (pv->reg[1]&8)) )
#else
    if(!skip && y<224)
#endif
      PicoLine(y);

    if(PicoOpt&1)
      sound_timers_and_dac(y);

    // get samples from sound chips
    if(y == 32 && PsndOut)
      emustatus &= ~1;
    else if((y == 224 || y == line_sample) && PsndOut)
      getSamples(y);

    // Run scanline:
    if (Pico.m.dma_xfers) SekCyclesBurn(CheckDMA());
    SekRun(CYCLES_M68K_LINE);
    Z80_RUN(CYCLES_Z80_LINE);
  }

  // V-int line (224 or 240)
  Pico.m.scanline=(short)y;

  // VDP FIFO
  pv->lwrite_cnt=0;
  Pico.video.status|=0x200;

  PAD_DELAY

  // Last H-Int:
  if (--hint < 0)
  {
    hint=pv->reg[10]; // Reload H-Int counter
    pv->pending_ints|=0x10;
    //printf("rhint: %i @ %06x [%i|%i]\n", hint, SekPc, y, SekCycleCnt);
    if (pv->reg[0]&0x10) SekInterrupt(4);
  }

  // V-Interrupt:
  pv->status|=0x08; // go into vblank
  pv->pending_ints|=0x20;

  // the following SekRun is there for several reasons:
  // there must be a delay after vblank bit is set and irq is asserted (Mazin Saga)
  // also delay between F bit (bit 7) is set in SR and IRQ happens (Ex-Mutants)
  // also delay between last H-int and V-int (Golden Axe 3)
  SekRun(CYCLES_M68K_VINT_LAG);
  if (pv->reg[1]&0x20) {
    elprintf(EL_INTS, "vint: @ %06x [%i]", SekPc, SekCycleCnt);
    SekInterrupt(6);
  }
  if (Pico.m.z80Run && (PicoOpt&4))
    z80_int();

  if (PicoOpt&1)
    sound_timers_and_dac(y);

  // get samples from sound chips
  if ((y == 224) && PsndOut)
    getSamples(y);

  // Run scanline:
  if (Pico.m.dma_xfers) SekCyclesBurn(CheckDMA());
  SekRun(CYCLES_M68K_LINE - CYCLES_M68K_VINT_LAG - CYCLES_M68K_ASD);
  Z80_RUN(CYCLES_Z80_LINE - CYCLES_Z80_ASD);

  // PAL line count might actually be 313 according to Steve Snake, but that would complicate things.
  lines = Pico.m.pal ? 312 : 262;

  for (y++;y<lines;y++)
  {
    Pico.m.scanline=(short)y;

    PAD_DELAY

    if(PicoOpt&1)
      sound_timers_and_dac(y);

    // Run scanline:
    if (Pico.m.dma_xfers) SekCyclesBurn(CheckDMA());
    SekRun(CYCLES_M68K_LINE);
    Z80_RUN(CYCLES_Z80_LINE);
  }

  // draw a frame just after vblank in alternative render mode
  if (!PicoSkipFrame && (PicoOpt&0x10))
    PicoFrameFull();

  return 0;
}

#undef PAD_DELAY
#undef Z80_RUN
