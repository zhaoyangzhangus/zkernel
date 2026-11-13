"""Production menu-close hover restoration, including stale ink-drop state."""
from pathlib import Path as _TestPath
import sys as _test_sys
_test_sys.path.insert(0, str(_TestPath(__file__).resolve().parents[1]))
from chromium_source import chromium_source
from pathlib import Path
import argparse
import resource
import subprocess
import tempfile
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-controls', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
src = (chromium_source() / 'ui/views/controls/button/menu_button_controller.cc').read_text()
start=src.index('void MenuButtonController::DecrementPressedLocked()')
method=src[start:src.index('\nvoid MenuButtonController::OnButtonStateChangedWhilePressedLocked',start)]
code=r'''
#include <cassert>
#define DCHECK_GE(A,B) assert((A)>=(B))
struct TimeTicks { static int Now() { return 42; } };
enum class InkDropState { DEACTIVATED };
struct Widget {
 bool drag=false;unsigned moves=0;
 bool *destroy_on_move=nullptr;
 bool dragged_view() { return drag; }
 void SynthesizeMouseMoveEvent() {
  ++moves;if(destroy_on_move)*destroy_on_move=false;
 }
};
struct Button {
 enum ButtonState { STATE_NORMAL,STATE_HOVERED,STATE_PRESSED,STATE_DISABLED };
 Widget widget; bool attached=true, alive=true, destroy=false;
 ButtonState state=STATE_PRESSED;
 Widget *GetWidget() { return attached?&widget:nullptr; }
 ButtonState GetState() { return state; }
 void SetState(ButtonState s) { state=s;if(destroy)alive=false; }
};
using LabelButton=Button;
struct Drop {
 bool hovered=true;unsigned sets=0,animations=0;
 bool *destroy_on_animation=nullptr;
 bool pressed_ripple=true;
 void SetHovered(bool v) { hovered=v;++sets; }
 void AnimateToState(InkDropState,void*) {
  pressed_ripple=false;++animations;if(destroy_on_animation)*destroy_on_animation=false;
 }
};
struct Delegate {
 Drop drop;bool should_hover=false;
 Drop *GetInkDrop() { return &drop; }
 bool ShouldEnterHoveredState() { return should_hover; }
};
static Drop *active_drop;
struct InkDrop { static Drop *Get(Button*) { return active_drop; } };
struct Weak { bool *alive;operator bool() { return *alive; } };
struct Factory { bool *alive;Weak GetWeakPtr() { return {alive}; } };
struct MenuButtonController {
 Button b;Delegate d;Factory weak_factory_{&b.alive};
 int pressed_lock_count_=1,menu_closed_time_=0,state_changed_subscription_=1;
 bool should_disable_after_press_=false;
 Button *button() { return &b; }
 Delegate *delegate() { return &d; }
 void DecrementPressedLocked();
};
''' + method + r'''
int main() {
 for(unsigned mode=0;mode<6;++mode) {
  MenuButtonController c;active_drop=&c.d.drop;
  c.d.should_hover=mode==1;c.b.widget.drag=mode==2;
  c.b.attached=mode!=3;c.should_disable_after_press_=mode==4;
  c.b.destroy=mode==5;
  c.DecrementPressedLocked();
  assert(!c.pressed_lock_count_ && !c.state_changed_subscription_);
  assert(c.d.drop.hovered==(mode==1) && c.d.drop.sets==1);
  assert(c.b.state==(mode==1?Button::STATE_HOVERED:mode==4?Button::STATE_DISABLED:Button::STATE_NORMAL));
  assert(c.d.drop.animations==((mode==3||mode==5)?0:1));
  assert(c.d.drop.pressed_ripple==(mode==3||mode==5));
  assert(c.b.widget.moves==(mode==1?1U:0U));
 }
 MenuButtonController nested;active_drop=&nested.d.drop;nested.pressed_lock_count_=2;
 nested.DecrementPressedLocked();assert(!nested.d.drop.sets && nested.b.state==Button::STATE_PRESSED);
 nested.DecrementPressedLocked();assert(!nested.d.drop.hovered && nested.b.state==Button::STATE_NORMAL);
 // 动画回调可能在悬停状态重新同步前销毁控制器。
 MenuButtonController animated;active_drop=&animated.d.drop;
 animated.d.should_hover=true;
 animated.d.drop.destroy_on_animation=&animated.b.alive;
 animated.DecrementPressedLocked();
 assert(!animated.b.alive && animated.d.drop.animations==1 && !animated.b.widget.moves);
 // 重新命中测试本身也可能销毁窗口部件；之后不得再访问它。
 MenuButtonController moved;active_drop=&moved.d.drop;moved.d.should_hover=true;
 moved.b.widget.destroy_on_move=&moved.b.alive;
 moved.DecrementPressedLocked();assert(!moved.b.alive && moved.b.widget.moves==1);
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-menu-hover-') as directory:
 p=Path(directory)
 def run_case(name, source, expected_failure=None):
  source_path=p/(name+'.cc'); binary=p/name
  source_path.write_text(source)
  subprocess.run(['c++','-std=c++20','-D__NATIVE_OS__','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                  '-fno-pie','-no-pie',str(source_path),'-o',str(binary)],check=True)
  def disable_core():
   resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
  result=subprocess.run([str(binary)],capture_output=True,text=True,preexec_fn=disable_core)
  if expected_failure is None:
   assert result.returncode == 0, result.stderr
  else:
   assert result.returncode != 0 and expected_failure in result.stderr, result.stderr
   print('Negative control rejected: '+name)
 run_case('production', code)
 if args.negative_controls:
  resync='button()->GetWidget()->SynthesizeMouseMoveEvent();'
  assert code.count(resync)==1
  run_case('missing-hover-resync', code.replace(resync, '(void)0;'), 'c.b.widget.moves==')
  restore='delegate()->GetInkDrop()->SetHovered(desired_state == Button::STATE_HOVERED);'
  assert code.count(restore)==1
  run_case('stale-outside-hover', code.replace(restore, 'delegate()->GetInkDrop()->SetHovered(true);'),
           'c.d.drop.hovered==')
print('Menu hover restoration: outside/inside, drag, disabled, detached, destroyed, nested locks, resync and callback destruction passed')
