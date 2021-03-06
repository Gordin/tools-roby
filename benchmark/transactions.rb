require 'roby'
require 'benchmark'

def random_plan(plan, num_tasks, num_task_relations, num_event_relations)
    num_tasks = num_tasks / 10 * 10
    num_tasks.times do
        plan.add(Roby::Task.new)
    end

    tasks = plan.known_tasks.to_a
    num_task_relations.times do
        from_group, to_group = 0, 0
        while from_group == to_group
            from_group = rand(tasks.size / 10)
            to_group   = rand(tasks.size / 10)
        end
        if from_group > to_group
            from_group, to_group = to_group, from_group
        end
        from_i = from_group * 10 + rand(10)
        to_i   = to_group * 10 + rand(10)

        from = tasks[from_i]
        to   = tasks[to_i]

        from.depends_on to
    end

    events = plan.free_events.to_a + plan.task_events.to_a
    created_edges = 0
    while created_edges < num_event_relations
        from_group, to_group = 0, 0
        while from_group == to_group
            from_group = rand(events.size / 10)
            to_group   = rand(events.size / 10)
        end
        if from_group > to_group
            from_group, to_group = to_group, from_group
        end
        from_i = from_group * 10 + rand(10)
        to_i   = to_group * 10 + rand(10)

        from = events[from_i]
        to   = events[to_i]
        if from.task != to.task
            begin
                from.forward_to to
                created_edges += 1
            rescue Roby::CycleFoundError
            end
        end
    end
end

COUNT = 1000
Benchmark.bm(70) do |x|
    plan = Roby::Plan.new
    (1..10).map do
        plan.add(Roby::Task.new)
    end
    tasks = plan.known_tasks.to_a
    plan.add(task = Roby::Task.new)

    x.report("creating #{COUNT} transactions") do
        COUNT.times do
            trsc = Roby::Transaction.new(plan)
            trsc.discard_transaction
        end
    end
    x.report("import non-connected task from plan (#{COUNT} times)") do
        COUNT.times do
            trsc = Roby::Transaction.new(plan)
            trsc[task]
            trsc.discard_transaction
        end
    end

    tasks[0].depends_on task
    task.depends_on tasks[1]
    task.start_event.forward_to tasks[2].stop_event
    tasks[3].stop_event.signals task.start_event

    x.report("import one connected task from plan (#{COUNT} times)") do
        COUNT.times do
            trsc = Roby::Transaction.new(plan)
            trsc[task]
            trsc.discard_transaction
        end
    end
    plan.clear

    plan = Roby::Plan.new
    Roby::ExecutionEngine.new(plan)
    random_plan(plan, 100, 120, 2000)

    x.report("import random plan iteratively (10 times and 100 tasks/plan)") do
        10.times do
            trsc = Roby::Transaction.new(plan)
            plan.known_tasks.each do |t|
                trsc[t]
            end
            trsc.discard_transaction
        end
    end
    plan.clear

    plan = Roby::Plan.new
    Roby::ExecutionEngine.new(plan)
    random_plan(plan, 100, 120, 2000)

    x.report("import random plan using find_tasks (10 times and 100 tasks/plan)") do
        10.times do
            trsc = Roby::Transaction.new(plan)
            trsc.find_tasks.to_a
            trsc.discard_transaction
        end
    end
    plan.clear
end

