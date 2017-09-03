/* vim:ts=4:sw=4:et:ai:sts=4
 *
 * python-unshare: Python bindings for the Linux unshare() syscall
 * Copyright © 2010 Martín Ferrari <martin.ferrari@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <Python.h>
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <sched.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <fcntl.h>

struct mapping {
    char **path;
    char *kw_name;
    int mask;
    char *mask_name;
    char *format;
    int is_mounted;
    int ns_old;
    int ns_new;
};

#ifndef CLONE_NEWCGROUP
# define CLONE_NEWCGROUP    0x02000000  /* New cgroup namespace.  */
#endif

static struct mask_list_entry {
    char *name;
    int mask;
} mask_list[] = {
    { "CLONE_FILES",     CLONE_FILES },
    { "CLONE_FS",        CLONE_FS },
    { "CLONE_NEWCGROUP", CLONE_NEWCGROUP },
    { "CLONE_NEWIPC",    CLONE_NEWIPC },
    { "CLONE_NEWNET",    CLONE_NEWNET },
    { "CLONE_NEWNS",     CLONE_NEWNS },
    { "CLONE_NEWPID",    CLONE_NEWPID },
    { "CLONE_NEWUSER",   CLONE_NEWUSER },
    { "CLONE_NEWUTS",    CLONE_NEWUTS },
    { "CLONE_SIGHAND",   CLONE_SIGHAND },
    { "CLONE_SYSVSEM",   CLONE_SYSVSEM },
    { "CLONE_THREAD",    CLONE_THREAD },
    { "CLONE_VM",        CLONE_VM },
    { NULL,              0 }
};

static PyObject * _unshare(PyObject *self, PyObject *args, PyObject *keywds)
{
    PyObject *result = NULL;
    int ret;

    int v_flags = 0;
    static char *empty = "";
    char *v_cgroup = empty;
    char *v_ipc = empty;
    char *v_mount = empty;
    char *v_net = empty;
    char *v_pid = empty;
    char *v_user = empty;
    char *v_uts = empty;
    static char *kwlist[] = { "flags", "cgroup", "ipc", "mount", "net", "pid", "user", "uts", NULL };
    struct mapping mapping[] = {
        { NULL, kwlist[0], 0, NULL, NULL, 0 },
        { &v_cgroup, kwlist[1], CLONE_NEWCGROUP, "CLONE_NEWCGROUP", "/proc/%d/ns/cgroup", 0, -1, -1 },
        { &v_ipc, kwlist[2], CLONE_NEWIPC, "CLONE_NEWIPC", "/proc/%d/ns/ipc", 0, -1, -1 },
        { &v_mount, kwlist[3], CLONE_NEWNS, "CLONE_NEWNS", "/proc/%d/ns/mnt", 0, -1, -1 },
        { &v_net, kwlist[4], CLONE_NEWNET, "CLONE_NEWNET", "/proc/%d/ns/net", 0, -1, -1 },
        { &v_pid, kwlist[5], CLONE_NEWPID, "CLONE_NEWPID", "/proc/%d/ns/pid_for_children", 0, -1, -1 },
        { &v_user, kwlist[6], CLONE_NEWUSER, "CLONE_NEWUSER", "/proc/%d/ns/user", 0, -1, -1 },
        { &v_uts, kwlist[7], CLONE_NEWUTS, "CLONE_NEWUTS", "/proc/%d/ns/uts", 0, -1, -1 },
        { NULL, NULL, 0, NULL, NULL, 0 }
    };

    if (!PyArg_ParseTupleAndKeywords(args, keywds, "|izzzzzzz", kwlist,
                                     &v_flags, &v_cgroup, &v_ipc, &v_mount,
                                     &v_net, &v_pid, &v_user, &v_uts)) {
        return NULL;
    }

    int flags_child = 0;
    int flags_parent = v_flags;
    pid_t my_pid = getpid();

    struct mapping *m;
    for ( m = mapping + 1 ; m->path ; m++) {
        if (*m->path != empty) {
            if (*m->path != NULL) {
                /* Bind mount needed, unshare in child */
                char ns[PATH_MAX];
                struct stat stat_buf;
                snprintf(ns, sizeof(ns), m->format, my_pid);
                ret = stat(ns, &stat_buf);
                if (ret == -1) {
                    PyErr_SetFromErrnoWithFilename(PyExc_OSError, ns);
                    goto err_file_missing;
                }
                flags_child |= m->mask;
                flags_parent &= ~m->mask;
            } else {
                /* Bind mount not needed, unshare in parent */
                flags_parent |= m->mask;
            }
        }
    }

    int fd[2] = { -1, -1 };
    if (flags_child) {
        ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fd);
        if(ret == -1) {
             PyErr_SetFromErrno(PyExc_OSError);
             goto err_socketpair;
        }
        pid_t pid = fork();
        if (pid < 0) {
            PyErr_SetFromErrno(PyExc_OSError);
            goto err_fork;
        } else if (pid == 0) {
            /* Child */
            close(fd[1]);
            unsigned char result = 0;
            ret = unshare(flags_child);
            if (flags_child & CLONE_NEWPID) {
                /* Force pid_for_children to appear */
                pid_t pid = fork();
                if (pid == 0) {
                    exit(0);
                }
                waitpid(pid, NULL, 0);
            }
            if (ret != 0) {
                result = errno < 255 ? errno : EINVAL;
            }
            if (write(fd[0], &result, 1) != 1) {
                goto exit_1;
            }
            ret = read(fd[0], &result, 1);
            if (ret != 1 || result != 'Q') {
                goto exit_1;
            }
            close(fd[0]);
            exit(0);
        exit_1:
            close(fd[0]);
            exit(1);
        } else {
            /* Parent */
            unsigned char result;

            close(fd[0]);
            fd[0] = -1;
            ret = read(fd[1], &result, 1);
            if (ret == -1) {
                PyErr_SetFromErrno(PyExc_OSError);
                goto out;
            }
            if (result != 0) {
                errno = result;
                PyErr_SetFromErrno(PyExc_OSError);
                goto out;
            }
            for ( m = mapping + 1 ; m->path ; m++) {;
                if (*m->path != NULL && *m->path != empty) {
                    char ns[PATH_MAX];
                    /* Save a restoration namespace in case of error */

                    snprintf(ns, sizeof(ns), m->format, my_pid);
                    m->ns_old = open(ns, O_RDONLY);
                    if (m->ns_old == -1) {
                        PyErr_SetFromErrnoWithFilename(PyExc_OSError, ns);
                        goto err_ns_old_open;
                    }
                    snprintf(ns, sizeof(ns), m->format, pid);
                    m->ns_new = open(ns, O_RDONLY);
                    if (m->ns_new == -1) {
                        PyErr_SetFromErrnoWithFilename(PyExc_OSError, ns);
                        goto err_ns_new_open;
                    }
                    ret = mount(ns, *m->path, NULL, MS_BIND, NULL);
                    if (ret == -1) {
                        close(fd[1]);
                        PyErr_SetFromErrnoWithFilename(PyExc_OSError, *m->path);
                        goto err_ns_mount;
                    } else {
                        m->is_mounted = 1;
                    }
                }
            }
            if (write(fd[1], "Q", 1) != 1) {
                PyErr_SetFromErrno(PyExc_OSError);
                goto err_child_terminate;
            }
            waitpid(pid, NULL, 0);
        }
    }
    if (flags_parent & ~CLONE_NEWUSER) {
        ret = unshare(flags_parent & ~CLONE_NEWUSER);
        if (ret == -1) {
            PyErr_SetFromErrno(PyExc_OSError);
            goto err_unshare_failed;
        }
    }
    for ( m = mapping + 1 ; m->path ; m++) {
        if (m->ns_new != -1) {
            ret = setns(m->ns_new, m->mask);
            if (ret == -1) {
                PyErr_SetFromErrno(PyExc_OSError);
                goto err_setns_failed;
            }
        }
    }
    if (flags_parent & CLONE_NEWUSER) {
        ret = unshare(CLONE_NEWUSER);
        if (ret == -1) {
            PyErr_SetFromErrno(PyExc_OSError);
            goto err_unshare_newuser_failed;
        }
    }
    if (flags_parent & CLONE_NEWPID) {
        /* Force pid_for_children to appear */
        pid_t pid = fork();
        if (pid == 0) {
            exit(0);
        }
        waitpid(pid, NULL, 0);
    }
    result = Py_None;
    goto out;


err_child_terminate:
err_unshare_newuser_failed:
err_setns_failed:
    /* restore old namespaces */
    for ( m = mapping + 1 ; m->path ; m++) {
        if (m->ns_old != -1) {
            ret = setns(m->ns_old, m->mask);
        }
    }
err_unshare_failed:
err_ns_mount:
err_ns_new_open:
err_ns_old_open:
    /* Remove (now invalid) ns mounts */
    for ( m = mapping + 1 ; m->path ; m++) {;
        if (m->is_mounted) {
            umount(*m->path);
            m->is_mounted = 0;
        }
    }
err_fork:
err_socketpair:
err_file_missing:

out:
    for ( m = mapping + 1 ; m->path ; m++) {;
        if (m->ns_old != -1) {
            close(m->ns_old);
            m->ns_old = -1;
        }
        if (m->ns_new != -1) {
            close(m->ns_new);
            m->ns_new = -1;
        }
    }
    if (fd[0] != -1) { close(fd[0]); }
    if (fd[1] != -1) { close(fd[1]); }
    if (result == NULL) {
        return NULL;
    } else {
        Py_RETURN_NONE;
    }
}

static PyObject * _unbind(PyObject *self, PyObject *args) {
    char *path;
    int nstype, ret;
    if (!PyArg_ParseTuple(args, "si", &path, &nstype))
        return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;;
    } else if (pid == 0) {
        /* Child */
        int fd;
        fd = open(path, O_RDONLY);
        if (fd == -1) {
            exit(1);
        }
        ret = setns(fd, nstype);
        if (ret == -1) {
            close(fd);
            exit(1);
        }
        close(fd);
        exit(0);
    } else {
        /* Parent */
        int status, res;
        res = waitpid(pid, &status, 0);
        if (res == -1) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }
        if (status != 0) {
            PyErr_Format(PyExc_OSError, "%s does not refer to namspace of kind %x", path, nstype);
            return NULL;
        }
        res = umount(path);
        if (res == -1) {
            PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

static PyObject * _setns(PyObject *self, PyObject *args) {
    int fd, nstype, ret;
    if (!PyArg_ParseTuple(args, "ii", &fd, &nstype))
        return NULL;
    ret = setns(fd, nstype);
    if(ret == -1)
        return PyErr_SetFromErrno(PyExc_OSError);
    Py_RETURN_NONE;
}

static PyMethodDef methods[] = {
    {"unshare", (PyCFunction)_unshare, METH_VARARGS | METH_KEYWORDS,
     "unshare(flags, **kwargs)\n\n"
     "Disassociate parts of the process execution context.\n"
     "flags is a bitmask that specifies which parts to unshare.\n\n"
     "Possible values for flags:\n"
     "  CLONE_VM CLONE_FS CLONE_FILES CLONE_SIGHAND CLONE_THREAD "
     "CLONE_NEWNS\n"
     "  CLONE_SYSVSEM CLONE_NEWUTS CLONE_NEWIPC CLONE_NEWUSER "
     "CLONE_NEWPID\n"
     "  CLONE_NEWNET\n"
     "Possible values for kwargs are (PATH == None is equivalent to FLAG):\n"
     "  cgroup=PATH    save new cgroup namespace to PATH (CLONE_NEWCGROUP)\n"
     "  ipc=PATH       save new ipc namespace to PATH (CLONE_NEWIPC)\n"
     "  mount=PATH     save new mount namespace to PATH (CLONE_NEWNS)\n"
     "  net=PATH       save new net namespace to PATH (CLONE_NEWNET)\n"
     "  pid=PATH       save new pid namespace to PATH (CLONE_NEWPID)\n"
     "  user=PATH      save new user namespace to PATH (CLONE_NEWUSER)\n"
     "  uts=PATH       save new uts namespa<ce to PATH (CLONE_NEWUTS)\n"
    },
    {"unbind", _unbind, METH_VARARGS,
     "unbind(path, nstype)\n\n"
     "Unbind path from its current namespace.\n"
     "  path is bound to a namespace.\n"
     "  nstype specifies the type of namespace that should be unbound.\n"
    },
    {"setns", _setns, METH_VARARGS,
     "setns(fd, nstype)\n\n"
     "Reassociate the calling thread with a new namespace.\n"
     "fd is a filedescriptor referring to a namespace.\n"
     "nstype specifies which type of namespace the calling thread\n"
     "may be reassociated with.\n\n"
     "Possible values for nstype:\n"
     "  0             Allow any type of namespace to be joined.\n"
     "  CLONE_NEWIPC  fd must refer to an IPC namespace.\n"
     "  CLONE_NEWNET  fd must refer to a network namespace.\n"
     "  CLONE_NEWNS   fd must refer to a mount namespace.\n"
     "  CLONE_NEWPID  fd must refer to a descendant PID namespace.\n"
     "  CLONE_NEWUSER fd must refer to a user namespace.\n"
     "  CLONE_NEWUTS  fd must refer to a UTS namespace.\n"
    },
    {NULL, NULL, 0, NULL}
};

#if PY_MAJOR_VERSION >= 3

static struct PyModuleDef moduledef = {
        PyModuleDef_HEAD_INIT,
        "unshare",
        NULL,
        0,
        methods,
        NULL,
        NULL,
        NULL,
        NULL
};

#define INITERROR return NULL

PyMODINIT_FUNC
PyInit_unshare(void)

#else

#define INITERROR return

PyMODINIT_FUNC initunshare(void)
#endif
{

#if PY_MAJOR_VERSION >= 3
    PyObject *m = PyModule_Create(&moduledef);
#else
    PyObject *m = Py_InitModule("unshare", methods);
#endif
    if (m == NULL)
        INITERROR;

    struct mask_list_entry *ml;

    for (ml = mask_list ; ml->name ; ml++) {
        PyModule_AddIntConstant(m, ml->name, ml->mask);
    }
#if PY_MAJOR_VERSION >= 3
    return m;
#endif
}

