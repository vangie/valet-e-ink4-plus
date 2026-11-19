"""Exercise sync ancestry and isolated merge with local git repositories; no network."""
import importlib.util
import json
import pathlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = pathlib.Path(__file__).resolve().parents[1]/'upstream/sync.py'
spec = importlib.util.spec_from_file_location('upstream_sync', SCRIPT)
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)

class SyncTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = pathlib.Path(self.temp.name)
        self.up = self.base/'official'
        self.local = self.base/'valet'
        self.up.mkdir()
        self.run_git(self.up,'init','-b','main')
        self.identity(self.up)
        (self.up/'CMakeLists.txt').write_text('set(PROJECT_VER "2.0.80")\n')
        (self.up/'official.txt').write_text('base\n')
        self.run_git(self.up,'add','.')
        self.run_git(self.up,'commit','-m','base')
        commit=self.run_git(self.up,'rev-parse','HEAD')
        self.run_git(self.base,'clone',str(self.up),str(self.local))
        self.identity(self.local)
        (self.local/'valet.txt').write_text('custom assistant\n')
        self.run_git(self.local,'add','.')
        self.run_git(self.local,'commit','-m','valet')
        self.config={'repository':str(self.up),'branch':'main','commit':commit,'sourceVersion':'2.0.80'}
        self.old_git=sync.git
        def call(*args,cwd=None,check=True):
            return self.old_git(*args,cwd=self.local if cwd is None else cwd,check=check)
        self.mock=patch.object(sync,'git',call)
        self.mock.start();self.addCleanup(self.mock.stop)

    def run_git(self,cwd,*args):
        return subprocess.check_output(['git',*args],cwd=cwd,text=True,stderr=subprocess.DEVNULL).strip()
    def identity(self,path):
        self.run_git(path,'config','user.email','sync-test@example.invalid')
        self.run_git(path,'config','user.name','Sync test')
    def advance(self):
        (self.up/'CMakeLists.txt').write_text('set(PROJECT_VER "2.0.81")\n')
        (self.up/'main/display').mkdir(parents=True)
        (self.up/'main/display/change.cc').write_text('upstream UI fix\n')
        self.run_git(self.up,'add','.')
        self.run_git(self.up,'commit','-m','v2.0.81')
    def test_current_source_no_fake_merge(self):
        d=sync.report(self.config,True)
        self.assertFalse(d['sourceUpdate'])
        self.assertFalse(sync.prepare(self.config,d,str(self.base/'merge'))['prepared'])
    def test_full_merge_keeps_valet_and_current_worktree(self):
        self.advance();head=self.run_git(self.local,'rev-parse','HEAD')
        d=sync.report(self.config,True)
        self.assertTrue(d['sourceUpdate']);self.assertTrue(d['uiReviewRequired'])
        result=sync.prepare(self.config,d,str(self.base/'merge'))
        self.assertEqual(result['conflicts'],[])
        self.assertTrue((self.base/'merge/valet.txt').exists())
        self.assertTrue((self.base/'merge/main/display/change.cc').exists())
        self.assertEqual(self.run_git(self.local,'rev-parse','HEAD'),head)
        self.assertFalse((self.local/'main/display/change.cc').exists())
        self.assertTrue(self.run_git(self.base/'merge','rev-parse','MERGE_HEAD'))
    def test_dirty_checkout_rejected_without_stash(self):
        self.advance();(self.local/'valet.txt').write_text('unsaved work\n')
        d=sync.report(self.config,True)
        with self.assertRaisesRegex(ValueError,'Commit'):sync.prepare(self.config,d,str(self.base/'merge'))
        self.assertEqual((self.local/'valet.txt').read_text(),'unsaved work\n')
    def test_conflict_left_in_isolated_worktree(self):
        (self.up/'official.txt').write_text('upstream\n')
        self.run_git(self.up,'add','.');self.run_git(self.up,'commit','-m','official edit')
        (self.local/'official.txt').write_text('valet edit\n')
        self.run_git(self.local,'add','.');self.run_git(self.local,'commit','-m','local edit')
        d=sync.report(self.config,True)
        result=sync.prepare(self.config,d,str(self.base/'conflict'))
        self.assertEqual(result['conflicts'],['official.txt'])
        self.assertEqual((self.local/'official.txt').read_text(),'valet edit\n')
    def test_rewritten_upstream_fails_closed(self):
        self.run_git(self.up,'checkout','--orphan','rewrite')
        self.run_git(self.up,'commit','-m','rewritten')
        self.config['branch']='rewrite'
        with self.assertRaisesRegex(ValueError,'diverged'):sync.report(self.config,True)

if __name__ == '__main__':unittest.main()
