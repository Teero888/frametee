#include "sm64_bridge.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <vector>

static char error[2048];
static void check(bool ok, const char *message) {
  if (!ok) { std::fprintf(stderr, "%s: %s\n", message, error); std::exit(1); }
}
static sm64_input input(int i) { return {uint16_t(i%7==0 ? 0x8000:0), int8_t(i%81-40), 65}; }
static sm64_view view(sm64_world *world) { sm64_view v{};sm64_ft_view(world,&v);return v; }
static bool equal(sm64_view a, sm64_view b) {
  return a.valid==b.valid && a.frame==b.frame && a.action==b.action && a.health==b.health &&
    !std::memcmp(a.pos,b.pos,sizeof(a.pos)) && !std::memcmp(a.vel,b.vel,sizeof(a.vel));
}
int main(int argc,char **argv) {
  if(argc!=2)return 64;
  auto *level=sm64_ft_open(argv[1],"/tmp",error,sizeof(error));check(level,"level");
  auto *a=sm64_ft_world_new(level,error,sizeof(error));
  auto *b=sm64_ft_world_new(level,error,sizeof(error));check(a&&b,"worlds");
  sm64_ft_world_set_scratch(a,true);
  sm64_view edit{};edit.health=0x500;
  check(sm64_ft_set(a,3,&edit,error,sizeof(error)),"scratch edit");
  sm64_ft_copy(b,a);
  for(int i=0;i<120;++i) {
    check(sm64_ft_step(a,input(i),error,sizeof(error)),"scratch step");
    check(sm64_ft_step(b,input(i),error,sizeof(error)),"reference step");
    check(equal(view(a),view(b)),"scratch survives interleaving and copy");
  }
  sm64_render_config config{};config.width=128;config.height=96;
  std::vector<uint8_t> plain(128*96*4), ghosts(plain.size());
  check(sm64_ft_render(a,&config,plain.data(),plain.size(),error,sizeof(error)),"scratch render");
  check(sm64_ft_step(a,input(120),error,sizeof(error)),"rendered step");
  check(sm64_ft_step(b,input(120),error,sizeof(error)),"reference continuation");
  check(equal(view(a),view(b)),"scratch render preserves continuation");

  sm64_scene_mario pose{};check(sm64_ft_pose(a,&pose),"native Mario pose");
  pose.pos[0]+=180;pose.pos[1]+=80;
  check(sm64_ft_render(a,&config,plain.data(),plain.size(),error,sizeof(error)),"plain scene");
  config.ghosts=&pose;config.ghost_count=1;
  check(sm64_ft_render(a,&config,ghosts.data(),ghosts.size(),error,sizeof(error)),"ghost scene");
  check(plain!=ghosts,"additional Mario changes scene pixels");
  config.ghost_count=0;
  std::vector<uint8_t> again(plain.size());
  check(sm64_ft_render(a,&config,again.data(),again.size(),error,sizeof(error)),"plain again");
  check(plain==again,"ghost rendering does not change active Mario");

  auto *first=sm64_ft_physics_create(a,error,sizeof(error));check(first,"isolated worker");
  auto *second=sm64_ft_physics_create(a,error,sizeof(error));check(second,"second worker");
  check(equal(*first->view,view(a)),"isolated source replay");
  auto *checkpoint=first->capture(first,error,sizeof(error));check(checkpoint,"checkpoint");
  check(!second->restore(second,checkpoint,error,sizeof(error)),"foreign checkpoint rejected");
  const auto start=std::chrono::steady_clock::now();
  auto future=std::async(std::launch::async,[&] { for(int i=121;i<421;++i)first->step(input(i)); });
  for(int i=121;i<421;++i) {
    second->step(input(i));
    check(sm64_ft_step(a,input(i),error,sizeof(error)),"editor alongside isolated workers");
  }
  future.get();
  check(equal(*first->view,*second->view)&&equal(*first->view,view(a)),"parallel/editor parity");
  auto final=*first->view;
  check(first->restore(first,checkpoint,error,sizeof(error)),"restore branch");
  for(int i=121;i<421;++i)first->step(input(i));
  check(equal(final,*first->view),"checkpoint continuation");
  std::printf("SM64 scratch, ghost, isolated workers, checkpoint and editor parity passed (%.3fs)\n",
    std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
  first->free_checkpoint(checkpoint);first->destroy(first);second->destroy(second);
  sm64_ft_world_free(a);sm64_ft_world_free(b);sm64_ft_level_free(level);
}
