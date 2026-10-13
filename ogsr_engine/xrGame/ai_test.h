#pragma once

// ⭐The combat AI autotest: a fight set up the same way every time, measured
// without a person at the keyboard, the numbers written to the log.
//
// WHY. "Better" for the siege was judged by the user watching one fight and by
// [tac] lines read afterwards -- and no two fights were the same (who was where,
// which squad, what he did). The plan (Боевой ИИ OGSR: от заплаток к симуляции,
// step 2) wants the goals as numbers that a change either moves or does not:
//   - every opening of the house under watch within 10 s of the siege beginning;
//   - nobody without a role standing farther than 15 m from the house;
//   - the first man in goes in through the door, and only once it is surrounded.
//
//     ai_test siege_izba [runs=1] [count=10] [time=90] [section=esc_stalker_respawn_2]
//                        [bearing=108] [dist=35] [keep=0] [at=x,y,z]
//         The player (made immortal for the run) into the izba of the Cordon
//         village; `count` stalkers of `section` spawned `dist` m off at
//         `bearing` degrees, hostile to him alone and told where he is; `time`
//         seconds of fight; the report; the stalkers removed (keep=1 leaves them).
//         `runs` > 1 does it again from scratch that many times and sums up:
//         the same fight differs run to run (06-10: the siege began at t=0 in
//         three runs of four, at t=72 in the fourth).
//         at= takes a smart terrain's name too (at=esc_lager).
//         kill=T [kill_from=10]: from kill_from s of the fight on, every T s the
//         nearest of them alive is shot dead by the player (a real GE_HIT in the
//         head from his weapon) -- and every death, his or one the fight made,
//         gets a line of the frames 2 s before it against 2 s after:
//             [ai_test] death 3 'esc_bandit_..' (player) t=34.0 | before avg 16.9 max 21 | after avg 19.2 max 74 >33ms 2
//         (07-10, the Switch: "the fps drops in a firefight, most when I kill one").
//     ai_test stop       ends the run now, with the report so far
//     ai_test status     one progress line
//
// ⚠The openings it scores against are the izba's door (a fact of the scenario)
// and the siege's own reading of the house (v1, rays in the fight): a window the
// siege missed is missed by the score too. Step 5 of the plan bakes the houses;
// the score moves onto that then.
namespace AITest
{
void Command(LPCSTR args);
void Frame(); // from CLevel::OnFrame
bool Running();
} // namespace AITest
